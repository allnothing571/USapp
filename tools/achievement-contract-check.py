# Strict YAML parse of the OpenAPI contract (Phase 4 B3, 0.45.0; derived from the B2 check), derived from p26r1-yaml-check.py. Docker Compose's loader is used
# as a parser only: the contract is embedded unchanged under an x- field and `config` returns it as JSON. Nothing is started. A
# negative control proves the loader rejects a duplicate key. This checks YAML structure and the presence of the B1/B2 operations,
# schemas and error codes; it does NOT validate OpenAPI semantics.
import json, os, subprocess, sys, tempfile

CONTRACT = 'D:/JYJ/USapp/contracts/openapi.yaml'
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


print(subprocess.run(['docker', 'compose', 'version'], capture_output=True, text=True).stdout.strip())
text = open(CONTRACT, encoding='utf-8', newline='').read()

dup = 'openapi: 3.1.0\npaths:\n  /a:\n    post:\n      operationId: one\n  /a:\n    delete:\n      operationId: two\n'
code, _, err = parse(dup)
print('negative control exit', code, '|', err.strip().splitlines()[-1][:160] if err.strip() else '')
if code == 0:
    sys.exit('PARSER NOT STRICT: duplicate key accepted; no conclusion can be drawn')

code, out, err = parse(text)
print('contract parse exit', code)
if code != 0:
    print(err.strip()[:800])
    sys.exit(2)
doc = json.loads(out)['x-contract']
paths = doc['paths']
METHODS = ('get', 'post', 'put', 'patch', 'delete')
ops = {}
for p, item in paths.items():
    for m in METHODS:
        if m in item:
            op = item[m].get('operationId', '?')
            assert op not in ops, ('duplicate operationId', op)
            ops[op] = f'{m} {p}'
print('openapi', doc['openapi'], 'version', doc['info']['version'], 'paths', len(paths), 'operations', len(ops))
assert doc['info']['version'] == '0.45.0'
expected = {
    'getAvatarCatalog': 'get /api/v1/avatar/catalog', 'getMyAvatar': 'get /api/v1/avatar',
    'purchaseAvatarItem': 'post /api/v1/avatar/purchases', 'saveAvatarLoadout': 'post /api/v1/avatar/loadout', 'listOwnedAvatarItems': 'get /api/v1/avatar/owned',
    'adminListAvatarAssets': 'get /api/admin/v1/avatar/assets', 'adminListAvatarItems': 'get /api/admin/v1/avatar/items',
    'adminCreateAvatarItem': 'post /api/admin/v1/avatar/items', 'adminGetAvatarItem': 'get /api/admin/v1/avatar/items/{itemId}',
    'adminReviseAvatarItem': 'post /api/admin/v1/avatar/items/{itemId}/revisions',
    'adminGetAvatarStarter': 'get /api/admin/v1/avatar/starter', 'adminSetAvatarStarter': 'post /api/admin/v1/avatar/starter',
}
for op, where in expected.items():
    assert ops.get(op) == where, (op, ops.get(op))
schemas = doc['components']['schemas']
for name in ('AvatarPurchaseRequest', 'AvatarPurchase', 'AvatarPurchaseResponse', 'AvatarOwnedListResponse', 'AvatarItem',
             'AvatarAppearanceResponse', 'AdminAvatarItem', 'AvatarLoadoutRequest', 'AvatarLoadoutResponse', 'AvatarLoadoutSlots'):
    assert name in schemas, name
assert 'owner_count' in schemas['AdminAvatarItem']['properties']
assert 'saved_loadout' in schemas['AvatarAppearanceResponse']['properties']['appearance']['properties']['source']['enum']
assert schemas['AvatarLoadoutRequest']['required'] == ['expected_version', 'slots']
assert schemas['AvatarPurchaseRequest']['required'] == ['item_id', 'expected_version']
codes = None
for name, schema in schemas.items():
    blob = json.dumps(schema)
    if 'AVATAR_ALREADY_OWNED' in blob and 'VALIDATION_ERROR' in blob:
        codes = blob
for code_name in ('AVATAR_ITEM_NOT_FOUND', 'AVATAR_VERSION_CONFLICT', 'AVATAR_ASSET_INVALID', 'AVATAR_STARTER_INVALID',
                  'AVATAR_ALREADY_OWNED', 'AVATAR_ITEM_UNAVAILABLE', 'AVATAR_SLOT_INVALID', 'AVATAR_NOT_OWNED', 'AVATAR_DEFAULTS_UNAVAILABLE'):
    assert codes and code_name in codes, code_name
# Every $ref resolves to a defined component.
refs = set()
def walk(node):
    if isinstance(node, dict):
        for k, v in node.items():
            if k == '$ref' and isinstance(v, str):
                refs.add(v)
            walk(v)
    elif isinstance(node, list):
        for v in node:
            walk(v)
walk(doc)
missing = []
for ref in refs:
    parts = ref.lstrip('#/').split('/')
    node = doc
    for part in parts:
        node = node.get(part) if isinstance(node, dict) else None
    if node is None:
        missing.append(ref)
assert not missing, missing
print('refs resolved', len(refs))
print('YAML CHECK PASSED (structure only; OpenAPI semantics not validated)')

assert ops.get('getAchievements') == 'get /api/v1/achievements'
assert 'AchievementSnapshot' in doc['components']['schemas']
print('ACHIEVEMENT CONTRACT CHECK PASSED')
