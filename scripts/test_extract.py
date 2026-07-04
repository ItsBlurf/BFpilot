import urllib.request
import urllib.parse
import json
import time
import zipfile

# Create a test zip file
with open('test.txt', 'w') as f:
    f.write('hello world\n' * 10)

with zipfile.ZipFile('test2.zip', 'w') as z:
    z.write('test.txt')

base_url = 'http://192.168.1.204:5905'

print("Uploading test2.zip...")
with open('test2.zip', 'rb') as f:
    data = f.read()
req = urllib.request.Request(base_url + '/api/fs/upload?path=/data&filename=test2.zip', data=data, method='POST')
req.add_header('Content-Type', 'application/octet-stream')
with urllib.request.urlopen(req) as r:
    print(json.loads(r.read().decode()))

print("Preparing job...")
body = b"src=/data/test2.zip&dst=/data/test2&password=&threads=0&nice=4&cleanupPartial=0"
req2 = urllib.request.Request(base_url + '/api/fs/archive/prepare', data=body, method='POST')
req2.add_header('Content-Type', 'application/x-www-form-urlencoded')
try:
    with urllib.request.urlopen(req2) as r:
        print(r.read().decode())
except Exception as e:
    print(e.read().decode())

def get(path):
    with urllib.request.urlopen(base_url + path) as r:
        return json.loads(r.read().decode())

print("Polling...")
for _ in range(15):
    try:
        st = get('/api/fs/archive/status')
        print(st.get('state'), st.get('current'), st.get('percent'))
        if st.get('state') in ['done', 'error']:
            break
    except Exception as e:
        print("poll err", e)
    time.sleep(1)
