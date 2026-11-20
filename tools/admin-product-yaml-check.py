# Strict YAML/ref checks for admin product inspection, contract 0.56.0.
# Compose is used only as a local parser; duplicate-key negative control; no OpenAPI semantic validation.
import json, os, re, subprocess, sys, tempfile

CONTRACT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'contracts', 'openapi.yaml'))
ENV = dict(os.environ, COMPOSE_DISABLE_ENV_FILE='1')
for name in ('COMPOSE_FILE', 'COMPOSE_PROFILES', 'COMPOSE_ENV_FILES'):
    ENV.pop(name, None)


def parse(text):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'compose.yaml')
        wrapped = 'name: yaml-check\nservices: {}\nx-contract:\n' + ''.join(
            ('  ' + line if line.strip() else line) for line in text.splitlines(keepends=True))
        open(path, 'w', encoding='utf-8', newline='\n').write(wrapped)
        r = subprocess.run(['docker', 'compose', '--project-directory', d, '-f', path, 'config', '--no-interpolate', '--format', 'json'],
                           capture_output=True, env=ENV)
        return r.returncode, r.stdout.decode('utf-8', 'replace'), r.stderr.decode('utf-8', 'replace').replace(path, '<file>')



text = open(CONTRACT,encoding='utf-8').read()
code,_,_ = parse('openapi: 3.1.0\npaths:\n  /a: {}\n  /a: {}\n')
assert code != 0, 'duplicate key accepted'
code,stdout,stderr=parse(text)
assert code==0,stderr
raw=json.loads(stdout); doc=raw['x-contract']
assert doc['info']['version']=='0.56.0'
refs=[]
def walk(v):
    if isinstance(v,dict):
        if '$ref' in v:
            ref=v['$ref']; assert ref.startswith('#/'); node=doc
            for key in ref[2:].split('/'): node=node[key.replace('~1','/').replace('~0','~')]
            refs.append(ref)
        for child in v.values():walk(child)
    elif isinstance(v,list):
        for child in v:walk(child)
walk(doc)
for suffix,op in [('', 'adminListCoupleProducts'),('/{productId}','adminGetCoupleProduct')]:
    assert doc['paths']['/api/admin/v1/couples/{coupleId}/products'+suffix]['get']['operationId']==op
for name in ['AdminProductSummary','AdminProductCurrent','AdminProductPage','AdminProductDetail']:
    assert doc['components']['schemas'][name]['additionalProperties'] is False
print('YAML/ref check passed; version=0.56.0; paths='+str(len(doc['paths']))+'; refs='+str(len(set(refs)))+'; duplicate control refused; OpenAPI semantics not validated')
