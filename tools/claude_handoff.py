#!/usr/bin/env python3
"""Private rendezvous for a cooperative Claude-to-Hardcore handoff.

No Terminal automation, process injection, transcript edits, or process kills.
The mod supplies the readiness decision; this helper prepares the receiving
window and correlates the new background job with a unique temporary name.
"""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time
import uuid
from urllib.parse import quote


TTL = 120
INTENT_TTL = 3600
MAX_RECORD = 1024 * 1024
MAX_REQUEST = 65536


def runtime_dir(create=True):
    root = Path.home()/'Library/Caches/hardcore/claude-handoff'
    if create:root.mkdir(parents=True, mode=0o700, exist_ok=True)
    try:info = root.lstat()
    except FileNotFoundError:
        if not create:return root
        raise
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError('handoff directory must be owned by you and private (0700)')
    return root


def request_path(request_id,create=True):
    if not re.fullmatch(r'[0-9a-f]{32}', request_id):
        raise ValueError('invalid request ID')
    return runtime_dir(create=create)/(request_id+'.json')


def write_json(path, value):
    temp = path.with_name(path.name+'.'+uuid.uuid4().hex+'.tmp')
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream); stream.flush(); os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def read_json(path):
    # O_NONBLOCK ensures a corrupt FIFO cannot stall the command guard before
    # fstat rejects it. Regular-file reads remain synchronous and bounded.
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError('unsafe handoff record')
        data = stream.read(MAX_RECORD + 1)
        if len(data) > MAX_RECORD: raise ValueError('handoff record exceeds the size limit')
        value = json.loads(data)
        if not isinstance(value, dict): raise ValueError('handoff record must be a JSON object')
        return value


@contextlib.contextmanager
def locked(path):
    fd = os.open(path.with_suffix('.lock'), os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if info.st_uid != os.getuid() or not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
            raise ValueError('unsafe handoff lock')
        deadline = time.monotonic() + .5
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline: raise ValueError('handoff is busy; retry')
                time.sleep(.01)
        yield
    finally:
        os.close(fd)


def jobs(command, cwd):
    result = subprocess.run([command, 'agents', '--json', '--all', '--cwd', cwd],
                            capture_output=True, text=True, timeout=10, check=True)
    entries = json.loads(result.stdout)
    if not isinstance(entries, list) or any(not isinstance(e, dict) for e in entries):
        raise ValueError('unsupported Claude session listing')
    return [e for e in entries if Path(e.get('cwd', '/')).resolve() == Path(cwd).resolve()]


def process_identity(pid):
    if type(pid) is not int or pid <= 1: return None
    result = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=', '-o', 'args='],
                            capture_output=True, text=True, timeout=.5)
    return result.stdout.strip() or None


def valid_cwd(value):
    if not isinstance(value, str) or not value.startswith('/'):
        raise ValueError('source directory must be an absolute path')
    cwd = Path(value).resolve(strict=True)
    if not cwd.is_dir(): raise ValueError('source directory is not a directory')
    return str(cwd)


def valid_session_id(value):
    if not isinstance(value, str): return False
    try: return str(uuid.UUID(value)) == value
    except ValueError: return False


def live_receiver(path):
    receiver = read_json(path.with_suffix('.receiver.json'))
    identity = receiver.get('identity')
    return isinstance(identity, str) and bool(identity) and process_identity(receiver.get('pid')) == identity


def require_same_window(record):
    identity=record.get('windowIdentity')
    if identity is not None and (not isinstance(identity,str) or not identity or process_identity(record.get('windowPid'))!=identity):
        raise ValueError('receiving window is no longer the prepared process')


def valid_source(data):
    sid = data.get('sourceSessionId', '')
    if not valid_session_id(sid):
        raise ValueError('invalid source session ID')
    return sid, valid_cwd(data.get('cwd'))


def valid_snapshot(value):
    """Validate the public mod API's deliberately limited transfer evidence.

    SessionMessage is a projection, not the entire persisted transcript. A
    matching digest must never become a claim that images, shell history, or
    terminal state were preserved. Keep that distinction in the wire format.
    """
    if not isinstance(value,dict) or set(value)!= {'draft','history'}:
        raise ValueError('invalid handoff state snapshot')
    draft,history=value.get('draft'),value.get('history')
    if not isinstance(draft,dict) or set(draft)!={'text','cursor'}:
        raise ValueError('invalid draft snapshot')
    text,cursor=draft.get('text'),draft.get('cursor')
    if not isinstance(text,str) or type(cursor) is not int:
        raise ValueError('invalid draft snapshot')
    encoded=text.encode('utf-16-le','surrogatepass');units=len(encoded)//2
    if not 0<=cursor<=units:
        raise ValueError('draft cursor is outside the saved text')
    if 0<cursor<units:
        before=int.from_bytes(encoded[(cursor-1)*2:cursor*2],'little')
        after=int.from_bytes(encoded[cursor*2:(cursor+1)*2],'little')
        if 0xD800<=before<=0xDBFF and 0xDC00<=after<=0xDFFF:
            raise ValueError('draft cursor splits a Unicode surrogate pair')
    if (not isinstance(history,dict) or not {'digest','count','complete'}<=set(history) or
            not set(history)<={'format','digest','count','complete','tail'}):
        raise ValueError('invalid history snapshot')
    if 'format' in history and history['format']!='api':raise ValueError('unsupported history snapshot format')
    if (not isinstance(history.get('digest'),str) or not re.fullmatch(r'[0-9a-f]{64}',history['digest']) or
            type(history.get('count')) is not int or not 0<=history['count']<=4096 or
            history.get('complete') is not False):
        raise ValueError('history must be a bounded public projection, not a complete-history claim')
    if 'tail' in history:
        tail=history['tail']
        if (history.get('format')!='api' or history['count']==0 or not isinstance(tail,dict) or
                set(tail)!={'blocks','textLength'} or type(tail.get('blocks')) is not int or not 1<=tail['blocks']<=65536 or
                (tail.get('textLength') is not None and (type(tail['textLength']) is not int or not 0<=tail['textLength']<=16*1024*1024))):
            raise ValueError('invalid exact history tail boundary')
    return value


def intent_path(session_id):
    if not valid_session_id(session_id):
        raise ValueError('invalid source session ID')
    return runtime_dir()/('intent-'+session_id+'.json')


def queue(data):
    """Request a loaded source mod's guarded handoff without typing commands.

    Delivery repeats until prepare binds the intent, so a failed poll or a mod
    reload cannot silently lose it. This is a request to wait for safe state,
    never permission to background an unchecked active turn.
    """
    sid, cwd = valid_source(data)
    command = shutil.which('claude')
    if not command: raise ValueError('Claude is not on PATH')
    sources = [e for e in jobs(command,cwd) if e.get('sessionId')==sid and e.get('kind')=='interactive']
    if len(sources)!=1: raise ValueError('the exact source is not one live interactive Claude session')
    source=sources[0]
    identity=process_identity(source.get('pid'))
    if not identity: raise ValueError('source process is no longer running')
    if 'expectedSourcePid' in data or 'expectedSourceIdentity' in data:
        if (type(data.get('expectedSourcePid')) is not int or
                data.get('expectedSourcePid')!=source.get('pid') or
                not isinstance(data.get('expectedSourceIdentity'),str) or
                data.get('expectedSourceIdentity')!=identity):
            raise ValueError('source process changed while its adapter was being configured; request a new move')
    path=intent_path(sid)
    with locked(path):
        if path.exists():
            old=read_json(path)
            if old.get('expires',0)>time.time() and old.get('state') in ('queued','prepared'):
                if old.get('sourceIdentity')!=identity or old.get('cwd')!=cwd:
                    raise ValueError('the pending handoff belongs to a different source process')
                return {'ok':True,'intentId':old['intentId'],'state':old['state'],
                        'expires':old['expires'],
                        **({'requestId':old['requestId']} if old.get('requestId') else {})}
        record={'protocol':1,'intentId':uuid.uuid4().hex,'sourceSessionId':sid,'cwd':cwd,
                'sourcePid':source['pid'],'sourceIdentity':identity,'created':time.time(),
                'expires':time.time()+INTENT_TTL,'state':'queued'}
        write_json(path,record)
    return {'ok':True,'intentId':record['intentId'],'state':'queued','expires':record['expires']}


def poll(data):
    sid,cwd=valid_source({'sourceSessionId':data.get('sessionId'),'cwd':data.get('cwd')})
    path=intent_path(sid)
    if not path.exists(): return {'ok':True,'action':None}
    with locked(path):
        record=read_json(path)
        if record.get('state')!='queued' or record.get('expires',0)<=time.time():
            return {'ok':True,'action':None}
        if record.get('sourceSessionId')!=sid or record.get('cwd')!=cwd:
            raise ValueError('queued handoff source mismatch')
        if process_identity(record.get('sourcePid'))!=record.get('sourceIdentity'):
            raise ValueError('queued handoff source process changed')
        return {'ok':True,'action':'wait','intentId':record['intentId']}


def bind_intent(data,request_id):
    intent_id=data.get('intentId')
    if intent_id is None:return
    if not isinstance(intent_id,str) or not re.fullmatch(r'[0-9a-f]{32}',intent_id):
        raise ValueError('invalid queued handoff identity')
    path=intent_path(data['sourceSessionId'])
    with locked(path):
        record=read_json(path)
        if (record.get('state')!='queued' or record.get('intentId')!=intent_id or
                record.get('expires',0)<=time.time() or record.get('cwd')!=valid_cwd(data.get('cwd'))):
            raise ValueError('queued handoff changed or expired')
        if process_identity(record.get('sourcePid'))!=record.get('sourceIdentity'):
            raise ValueError('queued handoff source process changed')
        record.update(state='prepared',requestId=request_id)
        write_json(path,record)


def cancel_intent(data):
    sid,cwd=valid_source({'sourceSessionId':data.get('sessionId'),'cwd':data.get('cwd')})
    path=intent_path(sid)
    if not path.exists():return {'ok':True,'intentId':None}
    with locked(path):
        record=read_json(path)
        if (('intentId' in data and record.get('intentId')!=data['intentId']) or record.get('cwd')!=cwd):
            raise ValueError('queued handoff identity mismatch')
        if record.get('state')=='prepared':
            raise ValueError('cancel the prepared handoff before cancelling its intent')
        record['state']='cancelled'
        write_json(path,record)
    return {'ok':True,'intentId':record['intentId']}


def prepare(data, executable):
    sid, cwd = valid_source(data)
    snapshot=valid_snapshot(data['snapshot']) if 'snapshot' in data else None
    if snapshot is not None and snapshot['history'].get('format')!='api':
        raise ValueError('Reload the updated Claude adapter with /reload-plugins before moving this session')
    command = shutil.which('claude')
    if not command: raise ValueError('Claude is not on PATH')
    entries = jobs(command, cwd)
    source = [e for e in entries if e.get('sessionId') == sid and e.get('kind') == 'interactive']
    if len(source) != 1: raise ValueError('the exact source is not one live interactive Claude session')
    source = source[0]
    identity = process_identity(source.get('pid'))
    if not identity: raise ValueError('source process is no longer running')
    root = runtime_dir()
    with locked(root/'registry.json'):
        for p in root.glob('*.json'):
            if not re.fullmatch(r'[0-9a-f]{32}\.json', p.name): continue
            old = read_json(p)
            if old.get('sourceSessionId') == sid and old.get('expires', 0) > time.time() and old.get('state') in ('prepared','armed','claimed','ready','attaching'):
                raise ValueError('this session already has a pending handoff')
        request_id = uuid.uuid4().hex
        path = request_path(request_id)
        record = {'protocol':1, 'requestId':request_id, 'sourceSessionId':sid,
                  'cwd':cwd, 'sourcePid':source['pid'], 'sourceIdentity':identity,
                  'claude':command, 'created':time.time(), 'expires':time.time()+TTL,
                  'marker':'Hardcore-move-'+request_id,
                  'originalName':source.get('name') or 'Claude',
                  'before':[e.get('sessionId') for e in entries], 'state':'prepared'}
        if snapshot is not None:record['snapshot']=snapshot
        write_json(path, record)
    try:
        result = subprocess.run([executable, '--claude-open', str(path),*(['--guard-input'] if snapshot is not None else [])],
                                capture_output=True, text=True, timeout=20, check=True)
        ready = json.loads(result.stdout)
        if not ready.get('ok'): raise ValueError('receiving window was not ready')
        window_pid=ready.get('pid')
        if type(window_pid) is not int or window_pid<=1:
            raise ValueError('receiving window did not identify itself')
        if snapshot is not None and ready.get('inputGuarded') is not True:
            raise ValueError('receiving window did not guard its input for draft restoration')
        window_identity=process_identity(window_pid)
        if not window_identity:raise ValueError('receiving window exited during preparation')
        with locked(path):
            record=read_json(path)
            record.update(windowPid=window_pid,windowIdentity=window_identity,executable=executable,inputGuarded=snapshot is not None)
            write_json(path,record)
        # The terminal client separately acknowledges its own startup.
        deadline = time.monotonic()+5
        receiver_path = path.with_suffix('.receiver.json')
        while not receiver_path.exists() and time.monotonic() < deadline: time.sleep(.05)
        if not live_receiver(path):
            raise ValueError('receiving client exited during preparation')
        bind_intent(data,request_id)
    except Exception:
        with locked(path):
            record = read_json(path); record['state']='cancelled'; write_json(path, record)
        raise
    return {'ok':True, **{k:record[k] for k in ('requestId','marker','originalName')}}


def arm_or_cancel(data, cancel=False):
    path = request_path(data.get('requestId',''))
    with locked(path):
        record = read_json(path)
        if data.get('sourceSessionId') != record['sourceSessionId']:
            raise ValueError('source session mismatch')
        if record['state'] not in ('prepared','armed'):
            raise ValueError('handoff is no longer pending')
        if not cancel:
            if record['expires'] < time.time(): raise ValueError('handoff expired')
            if not live_receiver(path):
                raise ValueError('receiving client is no longer available')
            require_same_window(record)
            if process_identity(record['sourcePid']) != record['sourceIdentity']:
                raise ValueError('source process changed')
            # No slow Claude subprocess inside the command guard. The mod
            # awaited rename; the destination must claim that exact marker and
            # explicitly acknowledge readiness before the receiver attaches.
        record['state']='cancelled' if cancel else 'armed'
        write_json(path, record)
    if cancel:
        # A queued intent must not silently reappear after /hardcore-slurp cancel.
        queued=intent_path(record['sourceSessionId'])
        if queued.exists():
            with locked(queued):
                intent=read_json(queued)
                if intent.get('requestId')==record['requestId']:
                    intent['state']='cancelled';write_json(queued,intent)
    return {'ok':True,'requestId':record['requestId']}


def destination(record, entries):
    matches = [e for e in entries if e.get('kind')=='background' and
        valid_session_id(e.get('sessionId')) and
        e.get('sessionId') not in record['before'] and
        e.get('name')==record['marker'] and isinstance(e.get('id'),str) and
        re.fullmatch(r'[0-9a-f]{8,36}',e['id'])]
    if len(matches)>1: raise ValueError('ambiguous destination; no session was attached')
    return matches[0] if matches else None


def bound_destination(record, entries):
    """After title restoration, retain the exact already-correlated identity."""
    sid, job_id = record.get('destinationSession'), record.get('jobId')
    if not valid_session_id(sid) or sid in record['before']: return None
    matches = [e for e in entries if e.get('kind') == 'background' and
               e.get('sessionId') == sid and e.get('id') == job_id]
    if len(matches) > 1: raise ValueError('ambiguous bound destination')
    return matches[0] if matches else None


def claim(data):
    sid = data.get('sessionId')
    if not valid_session_id(sid): raise ValueError('invalid destination session ID')
    cwd = valid_cwd(data.get('cwd'))
    for path in runtime_dir(create=False).glob('*.json'):
        if not re.fullmatch(r'[0-9a-f]{32}\.json',path.name): continue
        record=read_json(path)
        if record.get('state') not in ('armed','claimed') or record['expires']<=time.time() or record['cwd']!=cwd:continue
        # The Claude CLI may take seconds. Keep arm/cancel available while it
        # runs, then re-read the record under its lock before binding anything.
        entries=jobs(record['claude'],cwd)
        with locked(path):
            record=read_json(path)
            if record.get('state') not in ('armed','claimed') or record['expires']<=time.time() or record['cwd']!=cwd:continue
            target = bound_destination(record, entries) or destination(record, entries)
            if not target or target.get('sessionId')!=sid:continue
            if record.get('destinationSession')==sid:
                return claim_result(record)
            record.update(destinationSession=sid,jobId=target['id'],state='claimed')
            write_json(path,record)
            return claim_result(record)
    return {'ok':False,'reason':'No handoff for this session'}


def claim_result(record):
    return {'ok':True,'originalName':record['originalName'],'requestId':record['requestId'],
            **({'snapshot':record['snapshot']} if 'snapshot' in record else {})}


def live_destination(record, entries):
    """Require a live process, not just a newly announced background row."""
    target=bound_destination(record,entries)
    if not target:raise ValueError('the claimed destination is no longer available')
    if valid_cwd(target.get('cwd'))!=record['cwd']:
        raise ValueError('destination directory changed')
    if target.get('status') not in ('busy','waiting','idle'):
        raise ValueError('destination has not reported a live process status')
    identity=process_identity(target.get('pid'))
    if not identity:raise ValueError('destination process is no longer running')
    return target,identity


def ready(data):
    """Called by the destination mod after claim and awaited title restoration.

The CLI can announce a background row before its host is attachable. Attaching
at that point can enter Claude's stopped-session wake path. A destination-side
acknowledgement avoids guessing readiness from that early announcement.
    """
    sid=data.get('sessionId')
    if not valid_session_id(sid):raise ValueError('invalid destination session ID')
    path=request_path(data.get('requestId',''))
    record=read_json(path)
    if record.get('state') not in ('claimed','ready') or record.get('destinationSession')!=sid:
        raise ValueError('this destination has not claimed the handoff')
    entries=jobs(record['claude'],record['cwd'])
    with locked(path):
        record=read_json(path)
        if record.get('state') not in ('claimed','ready') or record.get('destinationSession')!=sid:
            raise ValueError('this destination has not claimed the handoff')
        if record['expires']<=time.time():raise ValueError('handoff expired')
        target,identity=live_destination(record,entries)
        if record['state']=='ready' and (record.get('destinationPid')!=target['pid'] or record.get('destinationIdentity')!=identity):
            raise ValueError('destination process changed after readiness')
        if not live_receiver(path):raise ValueError('receiving client is no longer available')
        require_same_window(record)
        record.update(state='ready',destinationPid=target['pid'],destinationIdentity=identity,
                      destinationReadyAt=record.get('destinationReadyAt',time.time()))
        write_json(path,record)
    return {'ok':True,'requestId':record['requestId']}


def native_receipt(record):
    """Ask the receiving Hardcore process to attest its own rendered PTY tab."""
    window_pid,receiver_pid=record.get('windowPid'),record.get('receiverPid')
    tty=record.get('receiverTty')
    if (type(window_pid) is not int or window_pid<=1 or type(receiver_pid) is not int or receiver_pid<=1 or
            not isinstance(tty,str) or not tty.startswith('/dev/') or any(c in tty for c in '\r\n\0')):
        raise ValueError('receiving terminal identity is unavailable')
    result=subprocess.run([record['executable'],'--claude-receipt',str(window_pid),record['requestId'],
                           str(receiver_pid),tty],capture_output=True,text=True,timeout=5,check=True)
    receipt=json.loads(result.stdout)
    expected={'ok':True,'requestId':record['requestId'],'windowPid':window_pid,
              'receiverPid':receiver_pid,'receiverTty':tty,'rendered':True}
    if not isinstance(receipt,dict) or any(type(receipt.get(k)) is not type(v) or receipt.get(k)!=v for k,v in expected.items()):
        raise ValueError('the receiving terminal did not verify the matching attached tab')
    return expected


def native_control(record,action):
    flags={'guard':('inputHeld',True),'cursor':('cursorSent',True),'release':('released',True)}
    if action not in flags:raise ValueError('invalid receiver control')
    window_pid,receiver_pid=record.get('windowPid'),record.get('receiverPid');tty=record.get('receiverTty')
    if (type(window_pid) is not int or window_pid<=1 or type(receiver_pid) is not int or receiver_pid<=1 or
            not isinstance(tty,str) or not tty.startswith('/dev/') or any(c in tty for c in '\r\n\0')):
        raise ValueError('receiving terminal identity is unavailable')
    result=subprocess.run([record['executable'],'--claude-'+action,str(window_pid),record['requestId'],
                           str(receiver_pid),tty],capture_output=True,text=True,timeout=5,check=True)
    reply=json.loads(result.stdout)
    expected={'ok':True,'requestId':record['requestId'],'windowPid':window_pid,
              'receiverPid':receiver_pid,'receiverTty':tty,flags[action][0]:flags[action][1]}
    if not isinstance(reply,dict) or any(type(reply.get(k)) is not type(v) or reply.get(k)!=v for k,v in expected.items()):
        raise ValueError('receiving terminal did not verify '+action)
    return expected


def receiver_control(data,action):
    sid=data.get('sessionId');path=request_path(data.get('requestId',''))
    if not valid_session_id(sid):raise ValueError('invalid destination session ID')
    record=read_json(path)
    if action in ('guard','cursor'):require_same_window(record)
    if (action=='guard' and record.get('state')=='ready' and record.get('destinationSession')==sid and
            record.get('inputGuarded') is True and record['expires']>time.time()):
        return {'ok':True,'waitingForAttach':True}
    verified_guard=action=='guard' and record.get('state')=='attached' and bool(record.get('snapshotVerifiedAt')) and bool(record.get('nativeReceipt'))
    allowed=('attaching','attached') if action in ('release','failed') or verified_guard else ('attaching',)
    if record.get('state') not in allowed or record.get('destinationSession')!=sid:
        raise ValueError('the matching destination is not attaching')
    if action in ('guard','cursor') and not verified_guard and (record['expires']<=time.time() or record.get('inputGuarded') is not True):
        raise ValueError('the receiver input guard is unavailable or expired')
    identity=record.get('destinationIdentity');pid=record.get('destinationPid')
    if action in ('guard','cursor'):
        entries=jobs(record['claude'],record['cwd'])
        target,live_identity=live_destination(record,entries)
        if target.get('pid')!=pid or live_identity!=identity:
            raise ValueError('destination process changed after readiness')
    with locked(path):
        current=read_json(path)
        if (current.get('state') not in allowed or current.get('destinationSession')!=sid or
                current.get('destinationIdentity')!=identity or current.get('destinationPid')!=pid):
            raise ValueError('destination changed while receiver control was pending')
        if verified_guard:
            receiver_identity=current.get('receiverIdentity')
            window_identity=current.get('windowIdentity')
            if (not current.get('snapshotVerifiedAt') or not current.get('nativeReceipt') or
                    not isinstance(receiver_identity,str) or not receiver_identity or
                    process_identity(current.get('receiverPid'))!=receiver_identity or
                    not isinstance(window_identity,str) or not window_identity or
                    process_identity(current.get('windowPid'))!=window_identity):
                raise ValueError('receiving client changed after verification')
            # This is not permission to fill again: the native input hold may
            # already be released, including after a lost release reply.
            return {'ok':True,'alreadyVerified':True}
        if action in ('guard','cursor') and current['expires']<=time.time():raise ValueError('handoff expired')
        if action=='failed':
            current['lastFailure']={'stage':'draft-restoration','reason':str(data.get('reason','Draft restoration failed.'))[:1024],'at':time.time()}
            write_json(path,current)
        reply=native_control(current,'release' if action=='failed' else action)
        if action in ('release','failed'):
            current['inputReleasedAt']=time.time()
            if action=='release' and current.get('lastFailure',{}).get('stage')=='input-release':current.pop('lastFailure',None)
            write_json(path,current)
    return reply


def verify(data):
    """Record exact destination-side draft and projected transcript equality.

    The native receipt proves output traversed the bound attached PTY and was
    rendered. It does not certify all Terminal scrollback or shell state, so
    these checks alone cannot authorize closing the source window.
    """
    sid=data.get('sessionId')
    if not valid_session_id(sid):raise ValueError('invalid destination session ID')
    snapshot=valid_snapshot(data.get('snapshot'))
    path=request_path(data.get('requestId',''))
    record=read_json(path)
    if record.get('state') not in ('attaching','attached') or record.get('destinationSession')!=sid:
        raise ValueError('the matching destination is not attaching')
    if record.get('snapshot')!=snapshot:
        raise ValueError('destination draft or history did not match the source')
    entries=jobs(record['claude'],record['cwd'])
    verified=record.get('state')=='attached' and bool(record.get('snapshotVerifiedAt')) and bool(record.get('nativeReceipt'))
    if verified:
        identity=record.get('receiverIdentity')
        window_identity=record.get('windowIdentity')
        if (not isinstance(identity,str) or not identity or process_identity(record.get('receiverPid'))!=identity or
                not isinstance(window_identity,str) or not window_identity or process_identity(record.get('windowPid'))!=window_identity):
            raise ValueError('receiving client changed after verification')
        receipt=record['nativeReceipt']
    else:receipt=native_receipt(record)
    with locked(path):
        record=read_json(path)
        if record.get('state') not in ('attaching','attached') or record.get('destinationSession')!=sid:
            raise ValueError('the matching destination is not attaching')
        if record.get('snapshot')!=snapshot:
            raise ValueError('destination draft or history did not match the source')
        if record['expires']<=time.time() and not verified:raise ValueError('handoff expired')
        target,identity=live_destination(record,entries)
        if record.get('destinationPid')!=target['pid'] or record.get('destinationIdentity')!=identity:
            raise ValueError('destination process changed after readiness')
        receiver_identity=process_identity(record.get('receiverPid'))
        if not receiver_identity:raise ValueError('receiving client exited during verification')
        window_identity=process_identity(record.get('windowPid'))
        if not window_identity:raise ValueError('receiving window exited during verification')
        if record.get('windowIdentity') and record['windowIdentity']!=window_identity:
            raise ValueError('receiving window changed during verification')
        if verified and receiver_identity!=record.get('receiverIdentity'):
            raise ValueError('receiving client changed after verification')
        if not verified:
            record.update(state='attached',snapshotVerifiedAt=time.time(),nativeReceipt=receipt,
                          receiverIdentity=receiver_identity,windowIdentity=window_identity)
            record.pop('lastFailure',None)
            write_json(path,record)
    if record.get('inputGuarded'):
        try:receiver_control(data,'release')
        except Exception as error:
            record_failure(path,'input-release',error)
            raise
    return {'ok':True,'requestId':record['requestId'],'draftVerified':True,
            'historyProjectionVerified':True,'clientAttachedVerified':True,'closureEligible':False}


def progress(data):
    path=request_path(data.get('requestId',''))
    record=read_json(path)
    return {'ok':True,'requestId':record['requestId'],'state':record['state'],
            'sourceSessionId':record['sourceSessionId'],
            'destinationSessionId':record.get('destinationSession'),
            'draftVerified':bool(record.get('snapshotVerifiedAt')),
            'historyProjectionVerified':bool(record.get('snapshotVerifiedAt')),
            'clientAttachedVerified':bool(record.get('nativeReceipt')),
            'terminalStateVerified':False,'shellStateVerified':False,
            'closureEligible':False}


def record_failure(path,stage,error):
    """Keep recovery evidence without turning a transient failure into a retry."""
    try:
        if path!=request_path(path.stem,create=False):return
        with locked(path):
            record=read_json(path)
            record['lastFailure']={'stage':stage,'reason':str(error)[:1024],'at':time.time()}
            write_json(path,record)
    except Exception:
        # A diagnostic must never mask the operation's actual failure.
        pass


def summary(record):
    state=record.get('state','unknown')
    expired=isinstance(record.get('expires'),(int,float)) and record['expires']<=time.time()
    status=('needs-recovery' if record.get('lastFailure') and state!='cancelled' else
            'expired' if expired and state not in ('attached','cancelled') else state)
    result={'kind':'request' if record.get('requestId') else 'intent','status':status,
            'state':state,'expired':expired,'created':record.get('created'),
            'expires':record.get('expires'),'sourceSessionId':record.get('sourceSessionId'),
            'project':record.get('cwd'),'liveChecked':False,'closureEligible':False,
            'draftVerifiedAt':record.get('snapshotVerifiedAt'),
            'historyProjectionVerifiedAt':record.get('snapshotVerifiedAt'),
            'clientVerifiedAt':record.get('snapshotVerifiedAt') if record.get('nativeReceipt') else None,
            'terminalStateVerified':False,'shellStateVerified':False}
    for name in ('requestId','intentId','destinationSession','jobId','lastFailure'):
        if name in record:result[name]=record[name]
    snapshot=record.get('snapshot')
    try:
        valid_snapshot(snapshot)
        draft=snapshot['draft']
        result['savedDraft']={'available':True,'utf16Units':len(draft['text'].encode('utf-16-le','surrogatepass'))//2,
                              'cursor':draft['cursor']}
    except ValueError:
        result['savedDraft']={'available':False}
    return result


def list_moves():
    """List historical metadata only: never print drafts or claim live status."""
    records=[];errors=[]
    for path in runtime_dir(create=False).glob('*.json'):
        request=bool(re.fullmatch(r'[0-9a-f]{32}\.json',path.name))
        intent=path.name.startswith('intent-') and valid_session_id(path.stem[7:])
        if not request and not intent:continue
        try:
            record=read_json(path)
            # A consumed intent is represented by its durable request below.
            if request and record.get('requestId')!=path.stem:raise ValueError('handoff record identity mismatch')
            if intent and record.get('requestId') and request_path(record['requestId'],create=False).exists():continue
            records.append(summary(record))
        except (OSError,ValueError,TypeError) as error:
            errors.append({'record':path.name,'error':str(error)})
    records.sort(key=lambda r:r.get('created') if type(r.get('created')) in (int,float) else 0,reverse=True)
    return {'ok':True,'moves':records,'unreadableRecords':errors,'liveChecked':False}


def inspect_move(request_id):
    """Observe current identities without attaching, waking, or changing state."""
    record=read_json(request_path(request_id,create=False))
    if record.get('requestId')!=request_id:raise ValueError('handoff record identity mismatch')
    result=summary(record)
    result.update(ok=True,liveChecked=True,sourceAvailable=False,destinationAvailable=False,
                  clientAvailable=False,observationErrors=[])
    identity=record.get('sourceIdentity')
    try:
        result['sourceAvailable']=isinstance(identity,str) and bool(identity) and process_identity(record.get('sourcePid'))==identity
    except Exception as error:result['observationErrors'].append(str(error))
    if record.get('destinationSession'):
        try:
            entries=jobs(record['claude'],record['cwd'])
            target,identity=live_destination(record,entries)
            result['destinationAvailable']=(target.get('pid')==record.get('destinationPid') and
                                            identity==record.get('destinationIdentity'))
            if not result['destinationAvailable']:
                result['observationErrors'].append('The destination process changed after readiness.')
        except Exception as error:
            result['observationErrors'].append(str(error))
        if result['destinationAvailable'] and record.get('state') in ('attaching','attached'):
            try:
                if record.get('snapshotVerifiedAt') and record.get('nativeReceipt'):
                    receiver_identity=record.get('receiverIdentity')
                    window_identity=record.get('windowIdentity')
                    result['clientAvailable']=(isinstance(receiver_identity,str) and bool(receiver_identity) and
                        process_identity(record.get('receiverPid'))==receiver_identity and
                        isinstance(window_identity,str) and bool(window_identity) and process_identity(record.get('windowPid'))==window_identity)
                else:
                    native_receipt(record)
                    result['clientAvailable']=True
            except Exception as error:
                result['observationErrors'].append(str(error))
    if result['destinationAvailable']:
        result['recovery']={'kind':'existing-live-job','jobId':record['jobId'],'project':record['cwd'],
                            'command':['claude','attach',record['jobId']]}
    elif result['sourceAvailable']:
        result['recovery']={'kind':'source-still-running'}
    else:
        result['recovery']={'kind':'inspect-claude-agents','project':record.get('cwd')}
    return result


def export_draft(request_id,destination):
    """Explicit, non-overwriting recovery of the exact saved Unicode draft."""
    record=read_json(request_path(request_id,create=False))
    if record.get('requestId')!=request_id:raise ValueError('handoff record identity mismatch')
    snapshot=valid_snapshot(record.get('snapshot'))
    destination=Path(destination).expanduser()
    temp=destination.with_name('.'+destination.name+'.'+uuid.uuid4().hex+'.tmp')
    value={'format':'hardcore-claude-draft-v1','requestId':record['requestId'],
           'sourceSessionId':record['sourceSessionId'],'destinationSessionId':record.get('destinationSession'),
           'project':record['cwd'],'draft':snapshot['draft']}
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    try:
        with os.fdopen(fd,'w') as stream:
            json.dump(value,stream,ensure_ascii=True,indent=2)
            stream.write('\n');stream.flush();os.fsync(stream.fileno())
        # Atomic publication refuses an existing file or symlink. No overwrite,
        # chmod of someone else's file, or partially written recovery artifact.
        os.link(temp,destination,follow_symlinks=False)
    finally:
        temp.unlink(missing_ok=True)
    return {'ok':True,'requestId':record['requestId'],'exportedDraft':str(destination.absolute()),
            'format':value['format'],'submitted':False}


def _wait_and_attach(path):
    # Reject arbitrary files supplied via the internal window argument.
    if path != request_path(path.stem): raise ValueError('invalid handoff record path')
    record=read_json(path)
    # Keep the receiver's recovery shell in the source project. Encode every
    # pathname control character so a directory name cannot inject terminal OSC.
    cwd=valid_cwd(record.get('cwd'))
    print('\x1b]7;file://localhost'+quote(cwd,safe='/')+'\x1b\\',end='',flush=True)
    identity=process_identity(os.getpid())
    if not identity: raise ValueError('could not identify the receiving client')
    write_json(path.with_suffix('.receiver.json'),{'pid':os.getpid(),'identity':identity})
    print('Hardcore is ready. Waiting for Claude to finish the guarded handoff.',flush=True)
    while time.time()<record['expires']:
        record=read_json(path)
        # The potentially slow CLI query runs outside the request lock, so the
        # guard's arm/cancel operation remains bounded.
        queried_ready=record['state']=='ready'
        entries=jobs(record['claude'],record['cwd']) if queried_ready else []
        with locked(path):
            record=read_json(path)
            if record['state']=='cancelled':
                print('Handoff cancelled. The original session was left available.',flush=True)
                return 1
            if queried_ready and record['state']=='ready' and record['expires']>time.time():
                target,identity=live_destination(record,entries)
                if record.get('destinationPid')!=target['pid'] or record.get('destinationIdentity')!=identity:
                    raise ValueError('destination process changed after readiness; no session was attached')
                require_same_window(record)
                record.update(state='attaching')
                record.update(receiverPid=os.getpid(),receiverTty=os.ttyname(0))
                write_json(path,record)
                print('Attaching to Claude '+target['id']+'…',flush=True)
                os.chdir(record['cwd'])
                os.execv(record['claude'],[record['claude'],'attach',target['id']])
        time.sleep(.25)
    record_failure(path,'receiver-timeout','The receiver did not attach before the handoff deadline.')
    print('Handoff timed out. Use hardcore claude inspect '+path.stem+' to check the source and existing background job.',flush=True)
    return 1


def wait_and_attach(path):
    try:return _wait_and_attach(path)
    except Exception as error:
        if isinstance(path,Path):record_failure(path,'receiver',error)
        raise


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['prepare','arm','cancel','claim','ready','verify','guard','cursor','release','failed','queue','poll','cancel-intent','progress','wait'])
    parser.add_argument('--executable')
    parser.add_argument('--request',type=Path)
    args=parser.parse_args()
    try:
        if args.action=='wait':return wait_and_attach(args.request)
        raw=sys.stdin.buffer.read(MAX_REQUEST+1)
        if len(raw)>MAX_REQUEST:raise ValueError('request exceeds the size limit')
        data=json.loads(raw)
        if not isinstance(data,dict) or data.get('protocol',1)!=1:raise ValueError('unsupported request')
        if args.action=='prepare':result=prepare(data,args.executable)
        elif args.action=='claim':result=claim(data)
        elif args.action=='ready':result=ready(data)
        elif args.action=='verify':result=verify(data)
        elif args.action in ('guard','cursor','release','failed'):result=receiver_control(data,args.action)
        elif args.action=='queue':result=queue(data)
        elif args.action=='poll':result=poll(data)
        elif args.action=='cancel-intent':result=cancel_intent(data)
        elif args.action=='progress':result=progress(data)
        else:result=arm_or_cancel(data,args.action=='cancel')
        print(json.dumps(result));return 0
    except Exception as error:
        if args.action=='wait':print('Hardcore: '+str(error),file=sys.stderr)
        else:print(json.dumps({'ok':False,'error':str(error)}))
        return 1


if __name__=='__main__':raise SystemExit(main())
