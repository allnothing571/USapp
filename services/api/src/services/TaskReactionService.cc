#include "services/TaskReactionService.h"
#include "services/MessageService.h"
#include "core/Crypto.h"
#include "db/Db.h"
#include <memory>

namespace usapp {
namespace {
using Service = TaskReactionService;
using Outcome = Service::Outcome;
#define RX_TS(x) "to_char(" x " AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
const char *kTarget =
 "SELECT m.couple_id::text,root.id::text AS root_id,root.creator_id::text,root.schedule_type,"
 "t.id::text AS stored_id,c.task_id::text AS completed_id "
 "FROM couple_members m LEFT JOIN tasks root ON root.couple_id=m.couple_id AND root.id=$2::uuid "
 "AND root.recurrence_id IS NULL LEFT JOIN tasks t ON "
 "((root.schedule_type IN ('daily','weekly') AND t.recurrence_id=root.id AND t.period_date=NULLIF($3,'')::date) "
 "OR (root.schedule_type NOT IN ('daily','weekly') AND t.id=root.id)) "
 "LEFT JOIN task_completions c ON c.task_id=t.id WHERE m.user_id=$1::uuid FOR SHARE OF m";
const char *kKey = "SELECT id::text,completion_task_id::text,request_fingerprint FROM task_reactions "
                   "WHERE author_id=$1::uuid AND idempotency_key=$2";
const char *kFacts =
 "SELECT json_build_object('id',r.id,'task_id',COALESCE(t.recurrence_id,t.id),"
 "'period_date',to_char(t.period_date,'YYYY-MM-DD'),'author_id',r.author_id,'kind',r.kind,"
 "'emoji_code',r.emoji_code,'body',r.body,'created_at'," RX_TS("r.created_at") ")::text AS facts "
 "FROM task_reactions r JOIN tasks t ON t.id=r.completion_task_id WHERE r.id=$1::uuid";
struct Context {
 std::shared_ptr<db::TxScope> scope;
 std::string user,root,key,couple,stored,id,fp;
 Service::Input input;
 Service::Callback done;
 bool committing=false,completed=false;
};
using Ctx=std::shared_ptr<Context>;
void complete(const Ctx &c, Service::Status result) {
 if(c->completed) return;
 c->completed=true; c->done(std::move(result));
}
void reject(const Ctx &c, Outcome outcome) {
 c->scope->rollback([c,outcome](){Service::Status s;s.outcome=outcome;complete(c,std::move(s));});
}
void fail(const Ctx &c) { c->scope->fail("task_reaction:query_failed"); }
void readFacts(const Ctx &c, bool replay) {
 c->scope->tx().execSqlAsync(kFacts,[c,replay](const drogon::orm::Result &rows){
  auto result=std::make_shared<Service::Status>();
  try {
   if(rows.size()!=1u) { fail(c);return; }
   Json::CharReaderBuilder reader;std::string error;
   std::unique_ptr<Json::CharReader> parser(reader.newCharReader());
   auto raw=rows[0]["facts"].as<std::string>();
   if(!parser->parse(raw.data(),raw.data()+raw.size(),&result->mutation,&error)) { fail(c);return; }
  } catch(const std::exception &) { fail(c);return; }
  std::string sql=std::string("SELECT ")+TaskService::taskColumnsSql()+
     " FROM tasks t LEFT JOIN task_completions c ON c.task_id=t.id WHERE t.id=$1::uuid";
  c->scope->tx().execSqlAsync(sql,[c,replay,result](const drogon::orm::Result &tasks){
   try {
    if(tasks.size()!=1u) { fail(c);return; }
    result->task=TaskService::taskFromRow(tasks[0]);
   } catch(const std::exception &) { fail(c);return; }
   result->outcome=replay?Outcome::Replayed:Outcome::Created;
   if(replay) { c->scope->rollback([c,result](){complete(c,std::move(*result));});return; }
   c->committing=true;c->scope->commit([c,result](){complete(c,std::move(*result));});
  },[c](const drogon::orm::DrogonDbException &){fail(c);},c->stored);
 },[c](const drogon::orm::DrogonDbException &){fail(c);},c->id);
}
void classify(const Ctx &c,std::function<void()> missing) {
 c->scope->tx().execSqlAsync(kKey,[c,missing](const drogon::orm::Result &rows){
  if(rows.empty()) { missing();return; }
  try {
   if(rows.size()!=1u) { fail(c);return; }
   if(rows[0]["request_fingerprint"].as<std::string>()!=c->fp) { reject(c,Outcome::KeyReused);return; }
   c->id=rows[0]["id"].as<std::string>();c->stored=rows[0]["completion_task_id"].as<std::string>();
  } catch(const std::exception &) { fail(c);return; }
  readFacts(c,true);
 },[c](const drogon::orm::DrogonDbException &){fail(c);},c->user,c->key);
}
void insert(const Ctx &c) {
 auto hex=crypto::randomHex(16);hex[12]='4';hex[16]='8';
 c->id=hex.substr(0,8)+"-"+hex.substr(8,4)+"-"+hex.substr(12,4)+"-"+hex.substr(16,4)+"-"+hex.substr(20);
 c->scope->tx().execSqlAsync(
  "INSERT INTO task_reactions(id,completion_task_id,couple_id,author_id,kind,emoji_code,body,idempotency_key,request_fingerprint) "
  "VALUES($1::uuid,$2::uuid,$3::uuid,$4::uuid,$5,NULLIF($6,''),NULLIF($7,''),$8,$9) "
  "ON CONFLICT DO NOTHING RETURNING id",
  [c](const drogon::orm::Result &rows){
   if(rows.empty()) { classify(c,[c](){reject(c,Outcome::AlreadyReacted);});return; }
   // Audit has no text or emoji body, only identity and kind.
   c->scope->tx().execSqlAsync(
    "INSERT INTO audit_logs(actor_type,actor_id,couple_id,action,target_type,target_id,detail) "
    "VALUES('user',$1::uuid,$2::uuid,'task.reacted','task_reaction',$3,"
    "json_build_object('task_id',$4::text,'kind',$5::text))",
    [c](const drogon::orm::Result &){readFacts(c,false);},
    [c](const drogon::orm::DrogonDbException &){fail(c);},c->user,c->couple,c->id,c->root,c->input.kind);
  },[c](const drogon::orm::DrogonDbException &){fail(c);},c->id,c->stored,c->couple,c->user,
  c->input.kind,c->input.emojiCode,c->input.body,c->key,c->fp);
}
}

bool TaskReactionService::parse(const Json::Value &body,Input &out,std::string &message) {
 out=Input{};
 if(!body.isObject() || !body["kind"].isString()) { message="请选择回应方式";return false; }
 out.kind=body["kind"].asString();
 for(const auto &key:body.getMemberNames()) {
  if(key!="kind" && key!="period_date" && !(out.kind=="emoji" && key=="emoji_code") && !(out.kind=="text" && key=="body")) {
   message="回应包含不支持的字段";return false;
  }
 }
 if(body.isMember("period_date")) {
  if(!body["period_date"].isString() || !TaskService::validPeriodDate(body["period_date"].asString())) {
   message="周期日期无效";return false;
  }
  out.periodDate=body["period_date"].asString();
 }
 if(out.kind=="emoji") {
  if(!body["emoji_code"].isString()) { message="请选择表情";return false; }
  out.emojiCode=body["emoji_code"].asString();
  if(out.emojiCode!="heart" && out.emojiCode!="clap" && out.emojiCode!="flower") { message="不支持的表情";return false; }
  return true;
 }
 if(out.kind!="text") { message="不支持的回应方式";return false; }
 Json::Value text;text["body"]=body["body"];
 if(!MessageService::parseReplyRequest(text,out.body,message)) return false;
 size_t points=0;for(unsigned char ch:out.body) if((ch&0xc0)!=0x80) ++points;
 if(points>40u) { message="回应最多 40 个字符";return false; }
 return true;
}
std::string TaskReactionService::fingerprint(const std::string &root,const Input &in) {
 auto field=[](const std::string &v){return std::to_string(v.size())+":"+v+";";};
 return crypto::sha256Hex("task-reaction:v1:"+field(root)+field(in.periodDate)+field(in.kind)+field(in.emojiCode)+field(in.body));
}
ErrorCode TaskReactionService::errorCode(Outcome out) {
 switch(out) {
  case Outcome::Validation:return ErrorCode::ValidationError;
  case Outcome::NotFound:return ErrorCode::TaskNotFound;
  case Outcome::NotCompleted:return ErrorCode::TaskNotCompleted;
  case Outcome::Forbidden:return ErrorCode::TaskReactionForbidden;
  case Outcome::AlreadyReacted:return ErrorCode::TaskAlreadyReacted;
  case Outcome::KeyReused:return ErrorCode::IdempotencyKeyReused;
  case Outcome::ResultUnknown:return ErrorCode::ResultUnknown;
  default:return ErrorCode::InternalError;
 }
}
void TaskReactionService::react(const std::string &user,const std::string &root,const std::string &key,
                               const Input &input,Callback cb) {
 if(!TaskService::validIdempotencyKey(key) || !TaskService::isCanonicalUuid(root) || !TaskService::isCanonicalUuid(user)) {
  Status s;s.outcome=Outcome::Validation;cb(std::move(s));return;
 }
 Json::Value request;request["kind"]=input.kind;
 if(input.kind=="emoji") request["emoji_code"]=input.emojiCode;else request["body"]=input.body;
 if(!input.periodDate.empty()) request["period_date"]=input.periodDate;
 Input normalized;std::string message;
 if(!parse(request,normalized,message)) { Status s;s.outcome=Outcome::Validation;cb(std::move(s));return; }
 if(!db::Pool::instance().ready()) { cb(Status{});return; }
 auto c=std::make_shared<Context>();c->user=user;c->root=root;c->key=key;c->input=normalized;c->done=std::move(cb);
 c->fp=fingerprint(root,normalized);std::weak_ptr<Context> weak=c;
 db::TxScope::begin([c](std::shared_ptr<db::TxScope> scope){
  c->scope=std::move(scope);
  c->scope->tx().execSqlAsync(kTarget,[c](const drogon::orm::Result &rows){
   if(rows.empty() || rows[0]["root_id"].isNull()) { reject(c,Outcome::NotFound);return; }
   try {
    if(rows.size()!=1u) { fail(c);return; }
    c->couple=rows[0]["couple_id"].as<std::string>();
    const auto schedule=rows[0]["schedule_type"].as<std::string>();
    bool recurring=schedule=="daily" || schedule=="weekly";
    if(recurring==c->input.periodDate.empty() || (schedule=="weekly" && !TaskService::isWeekStart(c->input.periodDate))) {
     reject(c,Outcome::Validation);return;
    }
    const bool creator=rows[0]["creator_id"].as<std::string>()==c->user;
    const bool completed=!rows[0]["completed_id"].isNull();
    c->stored=rows[0]["stored_id"].isNull()?"":rows[0]["stored_id"].as<std::string>();
    classify(c,[c,creator,completed](){
     if(!creator) { reject(c,Outcome::Forbidden);return; }
     if(!completed) { reject(c,Outcome::NotCompleted);return; }
     insert(c);
    });
   } catch(const std::exception &) { fail(c); }
  },[c](const drogon::orm::DrogonDbException &){fail(c);},c->user,c->root,c->input.periodDate);
 },[weak](const std::string &){if(auto c=weak.lock()) { Status s;s.outcome=c->committing?Outcome::ResultUnknown:Outcome::Error;complete(c,std::move(s)); }});
}
}
