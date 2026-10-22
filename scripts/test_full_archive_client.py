"""Fragmented IPC, bounded failure and reconnect without duplicate dispatch.
Run under Linux; it compiles the actual production client with ASan/UBSan.
"""
import errno,fcntl,json,os,socket,struct,subprocess,tempfile,threading,time
from pathlib import Path
project=Path(__file__).resolve().parents[1]
temporary=Path(tempfile.mkdtemp(prefix='bfpilot-full-client-'))
endpoint=temporary/'helper.sock';lock=temporary/'helper.lock';binary=temporary/'client'
subprocess.run(['gcc','-O1','-g','-fsanitize=address,undefined','-I'+str(project/'src'),
 '-DBFPILOT_FULL_ARCHIVE_SOCKET="'+str(endpoint)+'"','-DBFPILOT_FULL_ARCHIVE_LOCK="'+str(lock)+'"',
 str(project/'scripts/test_full_archive_client.c'),str(project/'src/full_archive_client.c'),'-o',str(binary)],check=True)
def frame(kind,identifier,payload=b''):
 return struct.pack('>4sHHIQ',b'B7HP',kind,0,len(payload),identifier)+payload
def read_exact(client,size):
 data=b''
 while len(data)<size:
  chunk=client.recv(size-len(data))
  if not chunk:raise EOFError('client closed')
  data+=chunk
 return data
def read_frame(client):
 magic,kind,flags,size,identifier=struct.unpack('>4sHHIQ',read_exact(client,20))
 assert magic==b'B7HP' and flags==0 and size<=1024*1024
 return kind,identifier,read_exact(client,size)
def fragmented(client,data):
 for i in range(0,len(data),3):client.sendall(data[i:i+3]);time.sleep(.0001)
def error(code,message):
 return struct.pack('>II',len(code),len(message))+code+message
def snapshot(source,destination):
 current=b'finished-file'
 return struct.pack('>IIIIQQIII',2,1,0,1,100,100,len(current),0,0)+current+struct.pack('>II',len(source),len(destination))+source+destination
results=[]
for case in ['success','cancel','no_space','busy','reconnect','mismatch','wrong_magic']:
 if endpoint.exists():endpoint.unlink()
 listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);listener.bind(str(endpoint));listener.listen(4);listener.settimeout(.2)
 lock_file=lock.open('w');fcntl.flock(lock_file,fcntl.LOCK_EX)
 stop=threading.Event();errors=[];counts={'extract':0,'attach':0,'ack':0,'cancel':0};task=None
 def server():
  global task
  while not stop.is_set():
   try:client,_=listener.accept()
   except socket.timeout:continue
   except OSError:break
   try:
    client.settimeout(4);kind,identifier,payload=read_frame(client)
    if kind==1:
     pong=frame(2,identifier,b'BF7Z/1 26.03-2-bf2')
     fragmented(client,(b'BAD!'+pong[4:]) if case=='wrong_magic' else pong)
    elif kind==10:
     counts['extract']+=1;task=identifier
     assert struct.unpack('>IIII',payload[:16])==(0,13,1,0)
     assert payload[16:29]==b'test-password'
     if case=='busy':fragmented(client,frame(25,identifier,error(b'busy',b'already running')));continue
     fragmented(client,frame(20,identifier))
     if case in ['reconnect','mismatch']:
      client.sendall(frame(21,identifier,struct.pack('>QQ',2,100))[:7]);continue
     fragmented(client,frame(21,identifier,struct.pack('>QQ',50,100)))
     current=b'file';fragmented(client,frame(22,identifier,struct.pack('>I',len(current))+current))
     if case=='cancel':
      kind,got_id,_=read_frame(client);assert kind==11 and got_id==identifier;counts['cancel']+=1
      fragmented(client,frame(25,identifier,error(b'canceled',b'cancelled')))
     elif case=='no_space':fragmented(client,frame(25,identifier,error(b'archive_no_space',b'no space')))
     else:fragmented(client,frame(24,identifier))
     try:read_frame(client);raise AssertionError('premature ACK on extraction connection')
     except EOFError:pass
    elif kind==14:
     counts['attach']+=1;assert identifier==task
     source=b'/owned/input.zip' if case=='reconnect' else b'/unrelated/input.zip'
     fragmented(client,frame(26,identifier,snapshot(source,b'/owned/private-stage')))
     if case=='reconnect':
      try:read_frame(client);raise AssertionError('premature ACK on recovery connection')
      except EOFError:pass
    elif kind==15:
     assert identifier==task;counts['ack']+=1
     fragmented(client,frame(24,identifier))
    else:raise AssertionError(kind)
   except (BrokenPipeError,ConnectionResetError):
    if case not in ['mismatch','wrong_magic']:errors.append('unexpected disconnect')
   except BaseException as exc:errors.append(repr(exc))
   finally:client.close()
 thread=threading.Thread(target=server);thread.start()
 process=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=15)
 stop.set();thread.join(5);listener.close();fcntl.flock(lock_file,fcntl.LOCK_UN);lock_file.close()
 expected={ 'success':(0,1),'cancel':(-errno.ECANCELED,1),'no_space':(-errno.ENOSPC,1),'busy':(-errno.EBUSY,1),
            'reconnect':(0,1),'mismatch':(-errno.EPROTO,0),'wrong_magic':(-errno.EPROTO,1)}[case]
 pass_=process.returncode==0 and not errors and ('rc=%d quiet=%d'%expected) in process.stdout and counts['extract']==(0 if case=='wrong_magic' else 1)
 if case in ['reconnect','mismatch']:pass_=pass_ and counts['attach']==1
 if case=='busy':pass_=pass_ and counts['ack']==0
 if case in ['success','cancel','no_space','reconnect']:pass_=pass_ and counts['ack']==1
 results.append({'case':case,'pass':pass_,'counts':counts,'stdout':process.stdout,'stderr':process.stderr,'serverErrors':errors})
 print(('PASS ' if pass_ else 'FAIL ')+case,flush=True)
 if not pass_:print(json.dumps(results[-1],indent=2),flush=True)
print(json.dumps({'results':results},indent=2))
endpoint.unlink();lock.unlink();binary.unlink();temporary.rmdir()
assert all(row['pass'] for row in results)
