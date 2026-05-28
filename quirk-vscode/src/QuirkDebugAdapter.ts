// QuirkDebugAdapter — VSCode Debug Adapter that bridges DAP <-> qdb (the
// stdin-driven interactive debugger inside the Quirk runtime).
//
// Architecture:
//   1. `launchRequest` spawns `quirk --debug <file>` with QUIRK_DBG_JSON=1.
//   2. qdb emits one JSON event per stderr line; we read line-by-line.
//   3. DAP requests (continue/next/stepIn/setBreakpoints/stackTrace/...) are
//      translated to qdb commands written to stdin. Commands that yield a
//      response (locals, bt, p, b, clear) are tracked via a promise queue
//      keyed by the expected event name.
//   4. Spontaneous events from qdb (stopped) drive DAP events back to VSCode.
//
// Path normalization: VSCode hands us absolute paths; qdb echoes back
// whatever the compiler stamped on the AST (often relative to the launch
// cwd). We resolve both to absolute paths against the launch cwd before
// comparing so gutter breakpoints survive the round trip.

import {
    DebugSession, InitializedEvent, StoppedEvent, TerminatedEvent,
    OutputEvent, Thread, StackFrame, Source, Scope, Variable, Breakpoint,
} from '@vscode/debugadapter';
import { DebugProtocol } from '@vscode/debugprotocol';
import { ChildProcessWithoutNullStreams, spawn } from 'child_process';
import * as path from 'path';

interface LaunchArgs extends DebugProtocol.LaunchRequestArguments {
    program: string;          // .quirk file to run
    compilerPath?: string;    // override the discovered quirk binary
    args?: string[];          // script-level argv
    cwd?: string;             // launch cwd; defaults to dirname(program)
    stopOnEntry?: boolean;    // if true, surface the first auto-pause to the user
    env?: { [k: string]: string };
    quirkHome?: string;       // QUIRK_HOME for the debuggee (stdlib resolution)
}

interface PendingResponse {
    event: string;                       // event name we're waiting for
    resolve: (payload: any) => void;
}

// Single fake thread id — Quirk is single-threaded today. VSCode insists
// every Stopped event names a thread, so we hand it the one we know about.
const QUIRK_THREAD_ID = 1;

export class QuirkDebugSession extends DebugSession {
    private child?: ChildProcessWithoutNullStreams;
    private launchCwd = process.cwd();
    private stopOnEntry = false;
    // True until we've seen the first auto-pause and resumed. While the run
    // is in this "warmup" window we delay configurationDone responses so
    // VSCode doesn't race ahead and send threads/stackTrace before the
    // debuggee has any frames pushed.
    private sawFirstStop = false;

    // Per DAP, the debuggee should NOT start executing until
    // `configurationDone` has fired. VSCode pushes setBreakpoints between
    // `initialized` and `configurationDone`; if we resumed the auto-pause
    // before configDone, breakpoints submitted in that window would land
    // in qdb too late to stop the program. We buffer the first stopped
    // event here and process it inside configurationDoneRequest.
    private configDone = false;
    private pendingFirstStop: any | null = null;

    // Buffered breakpoints — VSCode usually pushes setBreakpoints BEFORE
    // configurationDone, but qdb only reads stdin while paused. We hold
    // them until the first stop and then flush as `b <file>:<line>` lines.
    private pendingBreakpoints = new Map<string, number[]>();   // absPath -> lines

    // Request/response queue. qdb responses arrive in the same order as the
    // commands we sent, so a FIFO per event type is enough.
    private pending: PendingResponse[] = [];

    // Stitched stderr buffer — qdb writes one JSON object per line, but
    // node's chunk boundaries don't respect newlines.
    private stderrBuf = '';

    // The (file, line) from the most recent `stopped` event. The shadow
    // stack records each frame's *definition* line, not its currently-
    // executing line, so without overlaying this on top of `bt`'s frame 0
    // VSCode highlights the function's `define` line (or nothing, when
    // it's the synthetic main frame with line 0).
    private lastStopFile = '';
    private lastStopLine = 0;

    // ---- DAP request handlers ------------------------------------------

    protected initializeRequest(
        response: DebugProtocol.InitializeResponse,
        _args: DebugProtocol.InitializeRequestArguments,
    ): void {
        response.body = response.body || {};
        // We support exactly what qdb can do today. Hover-eval works via
        // the `p <name>` command; arithmetic in the watch panel does not.
        response.body.supportsConfigurationDoneRequest = true;
        response.body.supportsEvaluateForHovers = true;
        response.body.supportsStepBack = false;
        response.body.supportsRestartRequest = false;
        response.body.supportsTerminateRequest = true;
        response.body.supportsConditionalBreakpoints = false;
        response.body.supportsSetVariable = false;
        this.sendResponse(response);
        // Tell VSCode it can now send setBreakpoints / configurationDone.
        this.sendEvent(new InitializedEvent());
    }

    protected launchRequest(
        response: DebugProtocol.LaunchResponse,
        args: LaunchArgs,
    ): void {
        const program = args.program;
        if (!program) {
            this.sendErrorResponse(response, 1001, 'launch.program is required');
            return;
        }
        this.launchCwd = args.cwd || path.dirname(program);
        this.stopOnEntry = args.stopOnEntry === true;

        const binary = args.compilerPath || 'quirk';

        // Derive QUIRK_HOME so the debuggee can find runtime.so + stdlib.
        // Priority: explicit launch arg, then env, then walk up from the
        // compiler binary (bin/quirk → its parent). Without this the child
        // fails out with "Could not resolve module 'console'".
        let quirkHome = args.quirkHome || process.env.QUIRK_HOME;
        if (!quirkHome && args.compilerPath) {
            const binDir = path.dirname(args.compilerPath);
            if (path.basename(binDir) === 'bin') quirkHome = path.dirname(binDir);
        }

        const env: { [k: string]: string } = {
            ...process.env,
            ...(args.env || {}),
            QUIRK_DBG_JSON: '1',
        };
        if (quirkHome) env.QUIRK_HOME = quirkHome;

        const childArgs = ['--debug', program, ...(args.args || [])];

        try {
            this.child = spawn(binary, childArgs, { cwd: this.launchCwd, env });
        } catch (e: any) {
            this.sendErrorResponse(response, 1002, `spawn failed: ${e?.message ?? e}`);
            return;
        }

        this.child.stdout.on('data', (chunk: Buffer) => {
            // Program stdout (print, etc.) flows to the Debug Console.
            this.sendEvent(new OutputEvent(chunk.toString('utf8'), 'stdout'));
        });
        this.child.stderr.on('data', (chunk: Buffer) => this.consumeStderr(chunk));
        this.child.on('exit', (code) => {
            this.sendEvent(new OutputEvent(`\n[quirk exited with code ${code ?? 0}]\n`, 'console'));
            this.sendEvent(new TerminatedEvent());
        });
        this.child.on('error', (err) => {
            // ENOENT here typically means the configured compilerPath is
            // wrong or the quirk binary isn't on PATH — point the user
            // back at the relevant setting so they don't have to guess.
            const hint = (err as any).code === 'ENOENT'
                ? `\nset 'quirk.compilerPath' in settings or ensure 'quirk' is on PATH`
                : '';
            this.sendEvent(new OutputEvent(`quirk launch failed: ${err.message}${hint}\n`, 'stderr'));
            this.sendEvent(new TerminatedEvent());
        });

        this.sendResponse(response);
    }

    protected configurationDoneRequest(
        response: DebugProtocol.ConfigurationDoneResponse,
        _args: DebugProtocol.ConfigurationDoneArguments,
    ): void {
        this.configDone = true;
        this.sendResponse(response);
        // Process the deferred first-stop now that all setBreakpoints have
        // been delivered. If the child hasn't paused yet, onStopped will
        // run its first-stop path directly when the pause arrives.
        if (this.pendingFirstStop) {
            const evt = this.pendingFirstStop;
            this.pendingFirstStop = null;
            this.processFirstStop(evt);
        }
    }

    protected setBreakPointsRequest(
        response: DebugProtocol.SetBreakpointsResponse,
        args: DebugProtocol.SetBreakpointsArguments,
    ): void {
        const file = args.source.path
            ? path.resolve(args.source.path)
            : (args.source.name ?? '');
        const requestedLines = (args.breakpoints ?? []).map((b) => b.line);

        // Diff against what we already know — clear lines that were removed
        // and add the new ones. qdb tolerates both before-launch and after-
        // launch breakpoints; the flushing path on first stop handles the
        // pre-launch case.
        const prior = this.pendingBreakpoints.get(file) ?? [];
        const toClear = prior.filter((l) => !requestedLines.includes(l));
        const toAdd   = requestedLines.filter((l) => !prior.includes(l));
        this.pendingBreakpoints.set(file, requestedLines);

        if (this.child && this.sawFirstStop) {
            for (const ln of toClear) this.send(`clear ${file}:${ln}\n`);
            for (const ln of toAdd)   this.send(`b ${file}:${ln}\n`);
        }

        // Report all requested lines as verified — qdb accepts any line, we
        // can't actually validate it without parsing the source ourselves.
        response.body = {
            breakpoints: requestedLines.map((line) => {
                const bp = new Breakpoint(true, line) as DebugProtocol.Breakpoint;
                return bp;
            }),
        };
        this.sendResponse(response);
    }

    protected threadsRequest(response: DebugProtocol.ThreadsResponse): void {
        response.body = { threads: [new Thread(QUIRK_THREAD_ID, 'main')] };
        this.sendResponse(response);
    }

    protected async stackTraceRequest(
        response: DebugProtocol.StackTraceResponse,
        _args: DebugProtocol.StackTraceArguments,
    ): Promise<void> {
        const payload = await this.request('bt\n', 'stack');
        const frames: StackFrame[] = (payload.frames ?? []).map(
            (f: { name: string; file: string; line: number }, i: number) => {
                // Frame 0 is the active frame — overlay the stop event's
                // file/line so the editor highlights where we ACTUALLY
                // paused, not where the function was defined.
                let file = f.file;
                let line = f.line || 0;
                if (i === 0 && this.lastStopLine > 0) {
                    if (this.lastStopFile) file = this.lastStopFile;
                    line = this.lastStopLine;
                }
                const absFile = this.resolveSource(file);
                return new StackFrame(
                    i,
                    f.name || '?',
                    absFile ? new Source(path.basename(absFile), absFile) : undefined,
                    line,
                );
            });
        response.body = { stackFrames: frames, totalFrames: frames.length };
        this.sendResponse(response);
    }

    protected scopesRequest(
        response: DebugProtocol.ScopesResponse,
        _args: DebugProtocol.ScopesArguments,
    ): void {
        // One scope per frame: "Locals". `variablesReference = 1` is the
        // sentinel the variablesRequest handler looks for; we don't currently
        // support inspecting non-top frames (no `up`/`down` in qdb).
        response.body = { scopes: [new Scope('Locals', 1, false)] };
        this.sendResponse(response);
    }

    protected async variablesRequest(
        response: DebugProtocol.VariablesResponse,
        _args: DebugProtocol.VariablesArguments,
    ): Promise<void> {
        const payload = await this.request('locals\n', 'locals');
        const vars: Variable[] = (payload.items ?? []).map(
            (it: { name: string; value: string; type: string }) =>
                new Variable(it.name, it.value),
        );
        response.body = { variables: vars };
        this.sendResponse(response);
    }

    protected continueRequest(
        response: DebugProtocol.ContinueResponse,
        _args: DebugProtocol.ContinueArguments,
    ): void {
        this.send('c\n');
        response.body = { allThreadsContinued: true };
        this.sendResponse(response);
    }

    protected nextRequest(
        response: DebugProtocol.NextResponse,
        _args: DebugProtocol.NextArguments,
    ): void {
        this.send('n\n');
        this.sendResponse(response);
    }

    protected stepInRequest(
        response: DebugProtocol.StepInResponse,
        _args: DebugProtocol.StepInArguments,
    ): void {
        this.send('s\n');
        this.sendResponse(response);
    }

    protected stepOutRequest(
        response: DebugProtocol.StepOutResponse,
        _args: DebugProtocol.StepOutArguments,
    ): void {
        // No real step-out yet — `n` (step-over at the current frame) is the
        // closest approximation. The runtime would need a third step mode
        // ("stop when shadow_sp < entry_sp") to do this properly.
        this.send('n\n');
        this.sendResponse(response);
    }

    protected pauseRequest(
        response: DebugProtocol.PauseResponse,
        _args: DebugProtocol.PauseArguments,
    ): void {
        // qdb only pauses at instrumented statement boundaries; there's no
        // async interrupt today. If the program is in a hot loop without a
        // hook, we'd need a signal handler to honour this. Punt for now.
        this.sendResponse(response);
    }

    protected async evaluateRequest(
        response: DebugProtocol.EvaluateResponse,
        args: DebugProtocol.EvaluateArguments,
    ): Promise<void> {
        const expr = (args.expression || '').trim();
        // Identifier-only — qdb's `p` doesn't do arithmetic. Reject anything
        // that looks like more than a name so users get a clear message in
        // hover popups instead of a misleading "<no-addr>" response.
        if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(expr)) {
            response.body = { result: '<expression eval not supported>', variablesReference: 0 };
            this.sendResponse(response);
            return;
        }
        const payload = await this.request(`p ${expr}\n`, 'local');
        if (payload.missing) {
            response.body = { result: `<no local named ${expr}>`, variablesReference: 0 };
        } else {
            response.body = { result: String(payload.value ?? ''), variablesReference: 0 };
        }
        this.sendResponse(response);
    }

    protected disconnectRequest(
        response: DebugProtocol.DisconnectResponse,
        _args: DebugProtocol.DisconnectArguments,
    ): void {
        if (this.child && !this.child.killed) {
            // Try to bow out cleanly first so the runtime can run any
            // GC-relevant cleanup. SIGKILL after a beat if it doesn't go.
            try { this.child.stdin.write('q\n'); } catch { /* already closed */ }
            const childRef = this.child;
            setTimeout(() => { if (!childRef.killed) childRef.kill('SIGKILL'); }, 200);
        }
        this.sendResponse(response);
    }

    protected terminateRequest(
        response: DebugProtocol.TerminateResponse,
        _args: DebugProtocol.TerminateArguments,
    ): void {
        if (this.child && !this.child.killed) {
            try { this.child.stdin.write('q\n'); } catch { /* */ }
        }
        this.sendResponse(response);
    }

    // ---- qdb plumbing --------------------------------------------------

    private send(cmd: string): void {
        if (!this.child) return;
        try { this.child.stdin.write(cmd); } catch { /* child gone */ }
    }

    // Send a command and wait for the next event with the given name. The
    // adapter assumes commands and responses pair 1:1 in order, which qdb
    // honours (it processes one command per stdin line before reading the
    // next).
    private request(cmd: string, expectedEvent: string): Promise<any> {
        return new Promise((resolve) => {
            this.pending.push({ event: expectedEvent, resolve });
            this.send(cmd);
        });
    }

    private consumeStderr(chunk: Buffer): void {
        this.stderrBuf += chunk.toString('utf8');
        let nl: number;
        while ((nl = this.stderrBuf.indexOf('\n')) !== -1) {
            const line = this.stderrBuf.slice(0, nl).trim();
            this.stderrBuf = this.stderrBuf.slice(nl + 1);
            if (!line) continue;
            // Lines that don't start with `{` are loose stderr text — runtime
            // panics, libc warnings, etc. Surface them to the user as-is.
            if (!line.startsWith('{')) {
                this.sendEvent(new OutputEvent(line + '\n', 'stderr'));
                continue;
            }
            let evt: any;
            try { evt = JSON.parse(line); }
            catch {
                this.sendEvent(new OutputEvent(line + '\n', 'stderr'));
                continue;
            }
            this.handleEvent(evt);
        }
    }

    private handleEvent(evt: any): void {
        const kind = evt.event;
        if (kind === 'stopped') {
            this.onStopped(evt);
            return;
        }
        // Otherwise it's a response to a pending request. Match FIFO.
        const idx = this.pending.findIndex((p) => p.event === kind);
        if (idx >= 0) {
            const p = this.pending.splice(idx, 1)[0];
            p.resolve(evt);
            return;
        }
        // Unmatched events (`message`, etc.) go to the Debug Console so the
        // user sees them.
        if (kind === 'message' && evt.text) {
            this.sendEvent(new OutputEvent(`[qdb] ${evt.text}\n`, 'console'));
        }
    }

    private onStopped(evt: any): void {
        // Record where we are so stackTraceRequest can patch the top frame.
        this.lastStopFile = typeof evt.file === 'string' ? evt.file : '';
        this.lastStopLine = typeof evt.line === 'number' ? evt.line : 0;

        if (!this.sawFirstStop) {
            // If configurationDone hasn't arrived yet, stash this event and
            // wait — VSCode is still pushing setBreakpoints in the window
            // between `initialized` and `configurationDone`, and processing
            // the auto-pause now would race those.
            if (!this.configDone) {
                this.pendingFirstStop = evt;
                return;
            }
            this.processFirstStop(evt);
            return;
        }
        this.emitStopped(evt);
    }

    // Flush all queued breakpoints to qdb, then either resume (default) or
    // surface the pause to the user (if stopOnEntry was set OR the first
    // auto-pause line happens to be on one of the breakpoints we just
    // installed — without this we'd silently `c` past the user's breakpoint
    // because the runtime auto-pauses BEFORE the first user statement and
    // can't know about late-arriving breakpoints).
    private processFirstStop(evt: any): void {
        this.sawFirstStop = true;
        for (const [file, lines] of this.pendingBreakpoints) {
            for (const ln of lines) this.send(`b ${file}:${ln}\n`);
        }
        if (this.matchesPendingBreakpoint(this.lastStopFile, this.lastStopLine)) {
            this.emitStopped({ ...evt, reason: 'breakpoint' });
            return;
        }
        if (!this.stopOnEntry) {
            this.send('c\n');
            return;
        }
        this.emitStopped(evt);
    }

    // Compare the auto-pause location against the queued breakpoint set.
    // Paths from qdb may be relative (relative to the launch cwd) while
    // VSCode hands us absolute paths, so we resolve both to abs paths and
    // fall back to a basename compare — same tolerance qdb uses internally.
    private matchesPendingBreakpoint(file: string, line: number): boolean {
        if (!file || line <= 0) return false;
        const absStop = path.isAbsolute(file) ? file : path.resolve(this.launchCwd, file);
        for (const [bpFile, lines] of this.pendingBreakpoints) {
            if (!lines.includes(line)) continue;
            const absBp = path.isAbsolute(bpFile) ? bpFile : path.resolve(this.launchCwd, bpFile);
            if (absBp === absStop) return true;
            if (path.basename(absBp) === path.basename(absStop)) return true;
        }
        return false;
    }

    private emitStopped(evt: any): void {
        const reason: string = evt.reason || 'step';
        // Map qdb reasons to DAP reasons. VSCode treats "entry" specially
        // (shows "paused on entry" in the status bar).
        let dapReason: 'step' | 'breakpoint' | 'entry' | 'pause' = 'step';
        if (reason === 'breakpoint' || reason === 'userBreakpoint') dapReason = 'breakpoint';
        else if (reason === 'entry') dapReason = 'entry';
        this.sendEvent(new StoppedEvent(dapReason, QUIRK_THREAD_ID));
    }

    private resolveSource(file: string): string | undefined {
        if (!file) return undefined;
        if (path.isAbsolute(file)) return file;
        return path.resolve(this.launchCwd, file);
    }
}
