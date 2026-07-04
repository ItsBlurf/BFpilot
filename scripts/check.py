import urllib.request
import json
try:
    with urllib.request.urlopen('http://192.168.1.204:5905/api/fs/list?path=/data/test2') as r:
        print(json.dumps(json.loads(r.read().decode()), indent=2))
except Exception as e:
    print(e)
