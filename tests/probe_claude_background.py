#!/usr/bin/env python3
"""Opt-in live probe of retroactive Claude /bg task adoption.

Starts ordinary Claude on a fresh PTY (no Terminal.app UI automation, host,
tmux, or --bg at launch). Uses existing login and a disposable workspace.
Distinguishes preserving a shell task from preserving the Claude process.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import uuid
from test_mac_takeover import Session, wait, send
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from mac_exit_watch import ExitWatch


def write_json_atomic(path, value):
    temporary=path.with_name(path.name+'.tmp')
    temporary.write_text(json.dumps(value,indent=2))
    temporary.replace(path)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--foreground',action='store_true',help='start a foreground tool, then background it with Ctrl+B')
    parser.add_argument('--exit-code',type=int,choices=range(256),default=0,metavar='0..255')
    parser.add_argument('--direct-background',action='store_true',help='try /bg during a foreground tool without Ctrl+B')
    parser.add_argument('--process-tree',action='store_true',help='run a parent that waits for a separate worker process')
    parser.add_argument('--late-hooks',action='store_true',help='install task-state/status hooks only after the foreground task starts')
    args=parser.parse_args()
    if args.direct_background:args.foreground=True
    if args.late_hooks and (not args.foreground or args.direct_background):
        parser.error('--late-hooks requires --foreground and cannot use --direct-background')
    root = Path(tempfile.mkdtemp(prefix='hc-claude-bg-', dir='/tmp'))
    work = root/'work'; work.mkdir()
    command = shutil.which('claude')
    source_id = str(uuid.uuid4())
    env = dict(os.environ, TERM='xterm-256color', COLORTERM='truecolor')
    for key in list(env):
        if key.startswith('CLAUDE') or key in ('TMUX','HARDCORE_WINDOW_PID'):
            env.pop(key)
    job = work/'job.py'
    job.write_text('import os,time\nfrom pathlib import Path\n'
                   'p=Path(__file__).parent\n'
                   'with (p/"launches").open("a") as f:f.write(str(os.getpid())+"\\n")\n'
                   '(p/"pid").write_text(str(os.getpid()))\n'
                   f'for i in range({900 if args.direct_background else 450}):\n (p/"ticks").write_text(str(i));time.sleep(.1)\n'
                   '(p/"done").write_text(str(os.getpid()))\n'
                   'print("HCCLAUDECHILDDONE",flush=True)\n'
                   f'raise SystemExit({args.exit_code})\n')
    if args.process_tree:
        worker=work/'worker.py'
        worker.write_text(job.read_text().replace('"launches"','"worker-launches"')
                          .replace('"pid"','"worker-pid"').replace('"done"','"worker-done"'))
        job.write_text('import os,subprocess,sys\nfrom pathlib import Path\n'
                      'p=Path(__file__).parent\n'
                      'with (p/"launches").open("a") as f:f.write(str(os.getpid())+"\\n")\n'
                      '(p/"pid").write_text(str(os.getpid()))\n'
                      'result=subprocess.run([sys.executable,str(p/"worker.py")])\n'
                      '(p/"done").write_text(str(os.getpid()))\n'
                      'raise SystemExit(result.returncode)\n')
    task = shlex.join([sys.executable,str(job)])
    prompt = ('For this disposable test remember my favorite fruit is pineapple. '
              'Use Bash to run exactly '+task+' with run_in_background '+('false' if args.foreground else 'true')+'. '
              'Do not launch anything else. Once it is running, reply only HCCLAUDEREADY.')
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH',32,100,0,0))
    def setup():
        os.setsid();fcntl.ioctl(0,termios.TIOCSCTTY,0)
    hook_options=['--setting-sources','project','--settings','{}'] if args.late_hooks else ['--settings','{"disableAllHooks":true}']
    proc = subprocess.Popen([command,'--session-id',source_id,'--strict-mcp-config',
                             '--mcp-config','{"mcpServers":{}}',*hook_options,
                             '--no-chrome','--tools','Bash','--allowedTools','Bash('+task+')','--',prompt],
                            cwd=work,env=env,stdin=slave,stdout=slave,stderr=slave,preexec_fn=setup)
    os.close(slave)
    raw = bytearray()
    s = Session(root/'receiver'); short = None; child = None; watcher = None; worker_pid = None
    observed = None
    def screen():
        while select.select([master],[],[],0)[0]:
            try: data=os.read(master,65536)
            except OSError: break
            if not data:break
            raw.extend(data)
            # Minimal terminal query replies; this is not a slurp backend.
            for query,response in [(b'\x1b[6n',b'\x1b[1;1R'),(b'\x1b[c',b'\x1b[?1;2c'),
                                   (b'\x1b]11;?',b'\x1b]11;rgb:0000/0000/0000\x1b\\')]:
                if query in data:os.write(master,response)
        (root/'source.raw').write_bytes(raw)
        return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',raw.decode(errors='replace'))
    def pump_until(fn, label, timeout=60):
        return wait(lambda: (screen(),fn())[1],label,timeout)
    def type_source(text):
        os.write(master,text.encode());time.sleep(.3);os.write(master,b'\r')
    def jobs():
        result=subprocess.run([command,'agents','--json','--cwd',str(work)],
                              env=env,capture_output=True,text=True,timeout=15,check=True)
        return json.loads(result.stdout)
    try:
        time.sleep(2)
        initial=screen()
        if 'Yes,Itrustthisfolder' in re.sub(r'\s+','',initial):
            os.write(master,b'\x1b[B\r')
        pump_until(lambda:(work/'pid').exists(),'Claude started disposable child',90)
        child=int((work/'pid').read_text())
        watcher=ExitWatch(child)
        if args.process_tree:
            pump_until(lambda:(work/'worker-pid').exists(),'worker process starts',10)
            worker_pid=int((work/'worker-pid').read_text())
        if args.late_hooks:
            # Install only into this newly created disposable project, after the
            # command started. No launch-time hook, host, or user config change.
            config=work/'.claude';config.mkdir()
            hook=work/'status_hook.py'
            hook.write_text('''import json,re,sys,time
from pathlib import Path
root=Path(__file__).resolve().parent.parent
event=json.load(sys.stdin)
with (root/'hook-events.jsonl').open('a') as f:f.write(json.dumps(event)+'\\n')
prompt=event.get('prompt','')
if event.get('hook_event_name')!='UserPromptSubmit' or '<task-notification>' not in prompt:raise SystemExit()
if (root/'context-delivered.json').exists():raise SystemExit()
identity=json.loads((root/'hook-identity.json').read_text())
if event.get('session_id')!=identity.get('destinationSession'):raise SystemExit()
task_id=re.search(r'<task-id>(.*?)</task-id>',prompt)
tool_id=re.search(r'<tool-use-id>(.*?)</tool-use-id>',prompt)
if not task_id or task_id.group(1)!=identity.get('taskId'):raise SystemExit()
if not tool_id or tool_id.group(1)!=identity.get('toolUseId'):raise SystemExit()
output=re.search(r'<output-file>(.*?)</output-file>',prompt)
if not output:raise SystemExit()
path=Path(output.group(1))
if not path.exists() or 'HCCLAUDECHILDDONE' not in path.read_text():raise SystemExit()
deadline=time.monotonic()+3
while not (root/'kernel-exit.json').exists() and time.monotonic()<deadline:time.sleep(.02)
if not (root/'kernel-exit.json').exists():raise SystemExit()
status=json.loads((root/'kernel-exit.json').read_text())
if status.get('pid')!=identity['pid'] or status.get('kind')!='exit':raise SystemExit()
context=(f"Hardcore independently observed the exit of this disposable test command (PID {status['pid']}, task {identity['taskId']}, tool {identity['toolUseId']}) "
 f"using macOS NOTE_EXITSTATUS: actual exit code {status['exitCode']}. "
 "Claude's adopted-task -1 means its own exit status is unknown; it does not contradict this kernel observation. "
 "Keep the original notification unchanged. Use the independently observed status when explaining this test result. "
 "Do not rerun the command solely because of the unknown adopted-task status.")
result={'hookSpecificOutput':{'hookEventName':'UserPromptSubmit','additionalContext':context}}
(root/'context-delivered.json').write_text(json.dumps({'event':event,'result':result},indent=2))
print(json.dumps(result))
''')
            write_json_atomic(root/'hook-identity.json',{'pid':child,'sourceSession':source_id})
            handler={'hooks':[{'type':'command','command':shlex.join([sys.executable,str(hook)]),'timeout':5}]}
            (config/'settings.json').write_text(json.dumps({'hooks':{name:[handler] for name in ('Stop','UserPromptSubmit','PostToolUse')}}))
            time.sleep(2);screen()
        if args.foreground and not args.direct_background:
            pump_until(lambda:'ctrl+btoruninbackground' in re.sub(r'\s+','',screen()),
                       'foreground tool is ready to background',20)
            os.write(master,b'\x02')
            print('Sent Ctrl+B to background the already-running foreground tool',flush=True)
        # Avoid matching the supplied prompt: only the last rendered answer.
        if not args.direct_background:
            pump_until(lambda:screen().count('HCCLAUDEREADY')>=2,'background task acknowledged',45)
        if args.late_hooks:
            def ready_task():
                events=[]
                path=root/'hook-events.jsonl'
                if not path.exists():return None
                for line in path.read_text().splitlines():
                    try:events.append(json.loads(line))
                    except json.JSONDecodeError:continue
                posts=[e for e in events if e.get('session_id')==source_id and
                    e.get('hook_event_name')=='PostToolUse' and e.get('tool_name')=='Bash' and
                    e.get('tool_input',{}).get('command')==task and
                    e.get('tool_response',{}).get('backgroundTaskId')]
                if len(posts)!=1:return None
                post=posts[0];task_id=post['tool_response']['backgroundTaskId']
                if args.foreground and not post['tool_response'].get('backgroundedByUser'):return None
                for e in events:
                    if e.get('session_id')!=source_id or e.get('hook_event_name')!='Stop':continue
                    tasks=e.get('background_tasks',[])
                    if len(tasks)==1 and tasks[0].get('id')==task_id and tasks[0].get('status')=='running' and tasks[0].get('type')=='shell':
                        return {'pid':child,'sourceSession':source_id,'taskId':task_id,'toolUseId':post['tool_use_id']}
                return None
            identity=pump_until(ready_task,'exact task confirmed backgrounded in PostToolUse and Stop',15)
            write_json_atomic(root/'hook-identity.json',identity)
            print('PASS hook confirmed exact background task before handoff:',identity['taskId'],flush=True)
        pump_until(lambda:(work/'ticks').exists(),'tool is progressing',10)
        before=int((work/'ticks').read_text())
        print(f'Original Claude PID {proc.pid}; live child {child}; tick {before}',flush=True)
        # Identify any public, automatically available session endpoints without
        # inspecting unrelated user sessions or credentials.
        (root/'source-jobs.json').write_text(json.dumps(jobs(),indent=2))
        type_source('/bg')
        pump_until(lambda:proc.poll() is not None,'original Claude exits after /bg',45)
        listing=jobs();(root/'background-jobs.json').write_text(json.dumps(listing,indent=2))
        print('Background job metadata:',json.dumps(listing),flush=True)
        assert isinstance(listing,list) and len(listing)==1,listing
        entry=listing[0]
        assert Path(entry['cwd']).resolve()==work.resolve(),entry
        short=entry.get('short') or entry.get('id')
        assert short
        if args.late_hooks:
            identity['destinationSession']=entry['sessionId']
            write_json_atomic(root/'hook-identity.json',identity)
        assert not (work/'done').exists(), 'handoff only happened after the tool finished'
        os.kill(child,0)
        if worker_pid:
            os.kill(worker_pid,0)
            assert (work/'worker-launches').read_text().splitlines()==[str(worker_pid)]
        assert int((work/'ticks').read_text())>before
        assert (work/'launches').read_text().splitlines()==[str(child)]
        print('PASS /bg preserved original shell child, without relaunching it',flush=True)
        # A later revision may expose a different JSON shape: do not guess a
        # user job. Exactly one entry from this unique directory is required.
        fcntl.ioctl(s.slave,termios.TIOCSWINSZ,struct.pack('HHHH',32,100,0,0))
        receiver=subprocess.Popen([command,'attach',short],cwd=work,env=env,
                                  stdin=s.slave,stdout=s.slave,stderr=s.slave,preexec_fn=setup)
        s.pid=receiver.pid;s.stage();send(s.sender,{'go':True})
        os.close(s.master);s.master=-1;os.close(s.slave);s.slave=-1
        wait(s.control.exists)
        os.close(master);master=-1
        wait(lambda:(work/'done').exists(),'same task completes after source PTY closes',110)
        assert (work/'done').read_text()==str(child)
        assert (work/'launches').read_text().splitlines()==[str(child)]
        observed=watcher.wait(10)
        if worker_pid:
            assert (work/'worker-done').read_text()==str(worker_pid)
            assert (work/'worker-launches').read_text().splitlines()==[str(worker_pid)]
        assert observed and observed['kind']=='exit' and observed['exitCode']==args.exit_code,observed
        write_json_atomic(root/'kernel-exit.json',observed)
        print('Kernel exit status after handoff:',observed,flush=True)
        if args.late_hooks:
            wait((root/'context-delivered.json').exists,'independent status delivered with task notification',30)
            print('PASS retroactive hook delivered independent exit status on the task notification',flush=True)
        recall_prompt='What is my favorite fruit? Reply only with its uppercase name. Do not use tools.'
        if args.late_hooks:
            recall_prompt=('What is my favorite fruit, and what exit code did the independent macOS observer report? '
                           'Reply only with the uppercase fruit followed by HCSTATUS and that numeric exit code. Do not use tools.')
        s.ctl('type '+recall_prompt)
        time.sleep(.3);s.ctl('key 36 0 \\r')
        def recall():
            result=subprocess.run([command,'logs',short],env=env,capture_output=True,text=True,timeout=10)
            (root/'receiver.log').write_text(result.stdout)
            if not args.late_hooks:return 'PINEAPPLE' in result.stdout
            # Wait for the finished assistant response, not matching text while
            # it is still being rendered or echoed in the terminal input.
            for line in (root/'hook-events.jsonl').read_text().splitlines():
                try:event=json.loads(line)
                except json.JSONDecodeError:continue
                if event.get('hook_event_name')!='Stop' or event.get('session_id')!=entry['sessionId']:continue
                match=re.fullmatch(r'PINEAPPLE\s+HCSTATUS\s+(-?\d+)\s*',event.get('last_assistant_message',''))
                if match and int(match.group(1))==args.exit_code:return True
            return False
        wait(recall,'conversation recall from Hardcore',60)
        def completion():
            # Claude normalizes punctuation in project directory names. Resolve
            # only our exact session UUID rather than guessing that encoding.
            matches=list((Path.home()/'.claude/projects').glob('*/'+entry['sessionId']+'.jsonl'))
            if not matches:return None
            assert len(matches)==1, 'ambiguous transcript for test session'
            transcript=matches[0]
            for line in transcript.read_text().splitlines():
                try:record=json.loads(line)
                except json.JSONDecodeError:continue
                content=record.get('message',{}).get('content')
                if record.get('type')!='user' or not isinstance(content,str):continue
                output=re.search(r'<output-file>(.*?)</output-file>',content)
                code=re.search(r'completed \(exit code (-?\d+)\)',content)
                if output and code:
                    path=Path(output.group(1))
                    if path.exists() and 'HCCLAUDECHILDDONE' in path.read_text():
                        return {'reportedExitCode':int(code.group(1)),'outputPreserved':True}
            return None
        status=wait(completion,'completion notification and retained tool output',45)
        # Verify again after Claude interpreted the notification and completed
        # its final response: a late replay must not pass this probe.
        assert (work/'launches').read_text().splitlines()==[str(child)]
        if worker_pid:assert (work/'worker-launches').read_text().splitlines()==[str(worker_pid)]
        s.ctl('snap '+str(root/'receiver.bmp'));wait((root/'receiver.bmp').exists)
        (root/'result.json').write_text(json.dumps({'version':subprocess.check_output([command,'--version'],text=True).strip(),
            'sourcePid':proc.pid,'childPid':child,'sourceSession':source_id,'backgroundSession':entry.get('sessionId'),
            'startedForeground':args.foreground,'singleChildLaunch':True,'recall':True,
            'directBackground':args.direct_background,'workerPid':worker_pid,
            'lateHooks':args.late_hooks,'independentStatusUnderstood':args.late_hooks,
            'kernelExit':observed,**status},indent=2))
        print('PASS child PID, completion, output, and recall; reported exit code: '+str(status['reportedExitCode']),flush=True)
        if status['reportedExitCode']!=args.exit_code:
            print(f"LIMITATION: Claude reported {status['reportedExitCode']}, but the kernel observed {args.exit_code}",flush=True)
    except Exception as error:
        if watcher and observed is None:
            try:observed=watcher.wait(0)
            except OSError:pass
        evidence={'error':str(error),'sourcePid':proc.pid,'childPid':child,
                  'workerPid':worker_pid,'kernelExit':observed,
                  'directBackground':args.direct_background}
        for name in ('launches','worker-launches'):
            if (work/name).exists():evidence[name]=(work/name).read_text().splitlines()
        (root/'failure.json').write_text(json.dumps(evidence,indent=2))
        raise
    finally:
        if watcher:watcher.close()
        if master>=0:screen();os.close(master)
        if proc.poll() is None:proc.kill()
        proc.wait(timeout=10)
        for pid,script,finished in ((child,job,observed is not None),
                (worker_pid,work/'worker.py',observed is not None and (work/'worker-done').exists())):
            if not pid or finished:continue
            # A stale/reused PID must never target an unrelated process.
            cmd=subprocess.run(['ps','-p',str(pid),'-o','args='],capture_output=True,text=True).stdout
            if str(script) not in cmd:continue
            try:os.kill(pid,signal.SIGTERM)
            except ProcessLookupError:pass
        s.cleanup()
        if short:subprocess.run([command,'stop',short],env=env,capture_output=True,timeout=20)
        print('Private diagnostics:',root,flush=True)


if __name__=='__main__':main()
