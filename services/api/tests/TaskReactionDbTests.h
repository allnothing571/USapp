#pragma once
#include "MessageDbTests.h"
#include "WeeklyTaskDbTests.h"
#include "TaskChangeDbTests.h"
#include "services/TaskReactionService.h"
namespace usapptest::reaction {
namespace c=taskcompletion;namespace ts=taskservice;
using S=usapp::TaskReactionService;using O=S::Outcome;
inline std::shared_ptr<ts::Pending<S::Status>> start(const std::string &user,const std::string &id,const std::string &key,S::Input in={"emoji","heart","",""}) {
 auto p=std::make_shared<ts::Pending<S::Status>>();S::react(user,id,key,in,[p](S::Status s){p->deliver(std::move(s));});return p;
}
inline bool is(const std::optional<S::Status> &s,O out) {return s && s->outcome==out;}
inline void run(const c::ClientPtr &pool,const std::string &dsn) {
 const auto observer=c::Observer::connect(dsn);
 TEST_CASE("P2-4: author only, actual completion, same-key races, immutable facts and zero asset effects") {
  try {
   ts::CommittedSpace space,other;ts::commitSpace(pool,space,true);ts::commitSpace(pool,other,true);
   const auto id=c::publish(space.userA,"P2-4 completed",1,3);
   CHECK(is(start(space.userA,id,c::newUuid())->wait(),O::NotCompleted));
   CHECK(c::is(c::runComplete(space.userB,id,c::newUuid()),c::Outcome::Completed));
   const auto balance=c::walletOf(space.userB);
   CHECK(is(start(space.userB,id,c::newUuid())->wait(),O::Forbidden));
   CHECK(is(start(other.userA,id,c::newUuid())->wait(),O::NotFound));
   const auto key=c::newUuid();auto a=start(space.userA,id,key),b=start(space.userA,id,key);
   const auto x=a->wait(),y=b->wait();
   CHECK((is(x,O::Created)&&is(y,O::Replayed))||(is(y,O::Created)&&is(x,O::Replayed)));
   if(x&&y) {CHECK_EQ(x->mutation["id"].asString(),y->mutation["id"].asString());CHECK_EQ(x->task.reactionJson,y->task.reactionJson);}
   CHECK(is(start(space.userA,id,c::newUuid())->wait(),O::AlreadyReacted));
   CHECK(is(start(space.userA,id,key,{"emoji","clap","",""})->wait(),O::KeyReused));
   const auto read=ts::runGet(space.userB,id);CHECK(read.has_value());
   if(read) {const auto json=usapp::TaskService::taskJson(read->task);CHECK_EQ(json["reaction"]["author_id"].asString(),space.userA);}
   const auto after=c::walletOf(space.userB);CHECK(balance&&after);
   if(balance&&after) {CHECK_EQ(balance->flower,after->flower);CHECK_EQ(balance->sunshine,after->sunshine);}
   CHECK_EQ(c::completions(observer,id),1);CHECK_EQ(c::taskOps(observer,id),1);
   CHECK_EQ(c::scalar(observer,"SELECT count(*)::bigint AS n FROM task_reactions WHERE completion_task_id=$1::uuid",id),1);
   CHECK_EQ(c::scalar(observer,"SELECT count(*)::bigint AS n FROM audit_logs WHERE action='task.reacted' AND detail->>'task_id'=$1",id),1);
   // Different keys race on the completion identity, not on a shared client action.
   const auto second=c::publish(space.userA,"P2-4 race",1,0);c::runComplete(space.userB,second,c::newUuid());
   auto p=start(space.userA,second,c::newUuid()),q=start(space.userA,second,c::newUuid());auto p1=p->wait(),q1=q->wait();
   CHECK((is(p1,O::Created)&&is(q1,O::AlreadyReacted))||(is(q1,O::Created)&&is(p1,O::AlreadyReacted)));
  } catch(const std::exception &) { c::expectThat(false,"reaction","case failed (details withheld)"); }
 }
 TEST_CASE("P2-4: recurring period must be explicit; separate periods have separate identities") {
  try {
   ts::CommittedSpace s;ts::commitSpace(pool,s,true);
   for(const auto &schedule:{std::string("daily"),std::string("weekly")}) {
    const auto root=schedule=="daily"?daily::publish(s.userA,1):weekly::publish(s.userA,1);
    const auto date=schedule=="daily"?daily::day(observer):weekly::monday(observer);
    CHECK(is(start(s.userA,root,c::newUuid())->wait(),O::Validation));
    CHECK(c::is(daily::run(s.userB,root,date,c::newUuid()),c::Outcome::Completed));
    const auto key=c::newUuid();const auto reaction=start(s.userA,root,key,{"text","","谢谢你",date})->wait();
    CHECK(is(reaction,O::Created));if(reaction) {CHECK_EQ(reaction->mutation["task_id"].asString(),root);CHECK_EQ(reaction->mutation["period_date"].asString(),date);}
    CHECK(is(start(s.userA,root,key,{"text","","谢谢你",date})->wait(),O::Replayed));
    if(schedule=="weekly") CHECK(is(start(s.userA,root,c::newUuid(),{"emoji","heart","",weekly::monday(observer,0,1)})->wait(),O::Validation));
   }
   auto message=message::create(s.userA,"快捷回应");CHECK(message.has_value());
   if(message) {const auto replyKey=c::newUuid();auto reply=message::reply(s.userB,message->data.id,"❤️",replyKey);CHECK(message::is(message::reply(s.userB,message->data.id,"❤️",replyKey),message::MO::Replayed));CHECK(message::is(reply,message::MO::Created));CHECK(message::is(message::reply(s.userB,message->data.id,"谢谢"),message::MO::AlreadyReplied));}
   auto second=message::create(s.userA,"普通回复先发");if(second) {CHECK(message::is(message::reply(s.userB,second->data.id,"好呀"),message::MO::Created));CHECK(message::is(message::reply(s.userB,second->data.id,"👏"),message::MO::AlreadyReplied));}
  } catch(const std::exception &) {c::expectThat(false,"reaction-period","case failed (details withheld)");}
 }
 TEST_CASE("P2-4: two historical completions keep separate reactions after rollover and root cancellation") {
  try {
   ts::CommittedSpace s;ts::commitSpace(pool,s,true);
   for(const auto &schedule:{std::string("daily"),std::string("weekly")}) {
    task::TaskRow root;root.scheduleType=schedule;root.completionMode="count";root.targetCount=1;
    root.flowers=1;root.sunshine=0;root.createdAtSql="clock_timestamp()-interval '70 days'";
    const auto first=schedule=="daily"?daily::day(observer,-42):weekly::monday(observer,-6);
    const auto second=schedule=="daily"?daily::day(observer,-41):weekly::monday(observer,-5);
    auto tx=c::acquireTx(pool);daily::checkedExec(tx,task::taskSql(root,s.couple,s.userA,s.userB));
    // Reuse this isolated space; continue its existing flower chain between schedules.
    // The weekly iteration follows the daily iteration in the same user account: derive its starting chain.
    const auto existing=observer.client->execSqlSync("SELECT COALESCE(max(seq),0)::bigint AS seq,COALESCE((array_agg(balance_after ORDER BY seq DESC))[1],0)::bigint AS bal FROM wallet_ledger_entries WHERE user_id=$1::uuid AND asset_type='flower'",s.userB);
    long long seq=existing[0]["seq"].as<long long>(),bal=existing[0]["bal"].as<long long>();
    if(seq==0) daily::checkedExec(tx,wallet::accountSql(s.userB,s.couple,"flower"));
    for(const auto &date:{first,second}) {
     task::TaskRow child=root;child.id=c::newUuid();child.key=task::taskKey();child.scheduleType="one_off";
     child.recurrenceId=root.id;child.periodDateSql="'"+date+"'::date";
     const auto begin="('"+date+"'::date::timestamp AT TIME ZONE 'Asia/Shanghai')";
     child.createdAtSql=begin+"+interval '8 hours'";
     const auto span=schedule=="daily"?1:7;
     child.deadline=task::rawDeadline("('"+date+"'::date+"+std::to_string(span-1)+")","NULL::time","(('"+date+"'::date+"+std::to_string(span)+")::timestamp AT TIME ZONE 'Asia/Shanghai')");
     const auto op=c::newUuid();daily::checkedExec(tx,task::taskSql(child,s.couple,s.userA,s.userB));
     daily::checkedExec(tx,task::taskOperationSql(op,s.couple,s.userB,child.id));
     daily::checkedExec(tx,"INSERT INTO task_progress_events(task_id,couple_id,user_id,progress_count,target_count,idempotency_key,operation_id,recorded_at) VALUES('"+child.id+"','"+s.couple+"','"+s.userB+"',1,1,'"+c::newUuid()+"','"+op+"',"+begin+"+interval '12 hours')");
     daily::checkedExec(tx,task::completionSql(child.id,s.couple,s.userB,op,1,0,begin+"+interval '12 hours'"));
     daily::checkedExec(tx,wallet::entrySql(op,s.couple,s.userB,"flower",++seq,1,++bal));
    }
    daily::checkedExec(tx,"SET CONSTRAINTS ALL IMMEDIATE");auto committed=c::commitTx(tx);if(!committed||!*committed) throw std::runtime_error("historical fixture failed");
    const auto proposal=taskchange::propose(s.userA,root.id,taskchange::cancelInput());CHECK(taskchange::is(proposal,taskchange::CO::Proposed));
    CHECK(taskchange::is(taskchange::decide(s.userB,root.id,taskchange::idOf(proposal),usapp::TaskChangeService::Action::Approve),taskchange::CO::Decided));
    const auto a=start(s.userA,root.id,c::newUuid(),{"emoji","flower","",first})->wait();
    const auto b=start(s.userA,root.id,c::newUuid(),{"text","","做得很好",second})->wait();
    CHECK(is(a,O::Created));CHECK(is(b,O::Created));
    if(a&&b) {CHECK(a->mutation["id"]!=b->mutation["id"]);CHECK_EQ(a->task.periodDate,first);CHECK_EQ(b->task.periodDate,second);}
    const auto current=ts::runGet(s.userA,root.id);CHECK(current.has_value());if(current) CHECK(current->task.reactionJson.empty());
   }
  } catch(const std::exception &) {c::expectThat(false,"reaction-history","case failed (details withheld)");}
 }

}
}
