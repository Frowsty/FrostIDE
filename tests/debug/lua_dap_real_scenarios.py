# SPDX-License-Identifier: Apache-2.0
# The historical 30-scenario catalogue, with each verdict derived from a real FrostEditor Lua VM.
from __future__ import annotations

import math
import re
from typing import Any

from lua_dap_real_support import RealLuaSession, by_name, engine_event_cursor, numeric, physical_source, require, source_line, wait_engine_event

CATALOGUE = (
    ("handshake.initialize", "handshake", "initialize"),
    ("handshake.attach_and_configuration_done", "handshake", "configurationDone"),
    ("threads.single_lua_thread", "threads", "threads"),
    ("execution.pause_and_stopped_event", "execution", "pause"),
    ("stack.stack_trace_while_stopped", "stack", "stackTrace"),
    ("stack.stack_trace_paging", "stack", "stackTrace"),
    ("source.loaded_source_retrieval", "source", "source"),
    ("execution.continue_and_continued_event", "execution", "continue"),
    # Root-approved real owner workflow: detach/resume, then authenticated play.stop and Edit push.
    # The legacy loopback catalogue keeps its historical frost.stopPlay command and verdicts.
    ("lifecycle.owned_play_stop", "lifecycle", "play.stop"),
    ("lifecycle.disconnect", "lifecycle", "disconnect"),
    ("breakpoints.line", "breakpoints", "setBreakpoints"),
    ("breakpoints.multiple_lines", "breakpoints", "setBreakpoints"),
    ("breakpoints.conditional", "breakpoints", "setBreakpoints"),
    ("breakpoints.hit_condition", "breakpoints", "setBreakpoints"),
    ("breakpoints.logpoint", "breakpoints", "setBreakpoints"),
    ("breakpoints.pausepoint", "breakpoints", "setBreakpoints"),
    ("breakpoints.function", "breakpoints", "setFunctionBreakpoints"),
    ("breakpoints.exception", "breakpoints", "setExceptionBreakpoints"),
    ("stepping.step_over", "stepping", "next"),
    ("stepping.step_in", "stepping", "stepIn"),
    ("stepping.step_out", "stepping", "stepOut"),
    ("variables.scopes", "variables", "scopes"),
    ("variables.locals", "variables", "variables"),
    ("variables.upvalues", "variables", "variables"),
    ("variables.tables", "variables", "variables"),
    ("variables.set_variable", "variables", "setVariable"),
    ("evaluate.watch", "evaluate", "evaluate"),
    ("evaluate.repl", "evaluate", "evaluate"),
    ("coroutines.multi_coroutine_threads", "coroutines", "threads"),
    ("lifecycle.terminate", "lifecycle", "terminate"),
)


def fixture_oracle(source: str) -> dict[str, float]:
    """Read constants from the frozen loaded Lua bytes; never assume stack IDs, lines or handles."""
    captured = re.search(r"local captured = (\d+)\r?\n", source)
    payload = re.search(r"local payload = \{ speed = (\d+), dt = ([0-9.]+),", source)
    child = re.search(r"local childValue = (\d+)\r?\n", source)
    answer = re.search(r"nested = \{ answer = (\d+) \}", source)
    require(captured and payload and child and answer, "actual installed conformance fixture changed its oracle grammar")
    return {"captured": int(captured[1]), "speed": int(payload[1]), "dt": float(payload[2]),
            "childValue": int(child[1]), "answer": int(answer[1])}


def scoped(session: RealLuaSession, stop: dict[str, Any], name: str) -> tuple[int, list[dict[str, Any]]]:
    scopes = session.scopes(stop)
    matches = [scope for scope in scopes if name.casefold() in scope.get("name", "").casefold()]
    require(len(matches) == 1 and matches[0].get("variablesReference", 0) > 0,
            f"real VM omitted its live {name} scope handle")
    reference = matches[0]["variablesReference"]
    return reference, session.variables(reference)


def stopped_at(session: RealLuaSession, marker: str = "R7_LOCALS") -> dict[str, Any]:
    session.breakpoints([marker])
    session.start()
    stop = session.stopped(marker)
    if marker != "R7_ON_CREATE":
        session.await_play_started()
    return stop


def continue_to(session: RealLuaSession, stop: dict[str, Any], marker: str) -> dict[str, Any]:
    session.breakpoints([marker])
    after = session.dap.event_cursor
    session.dap.request("continue", {"threadId": stop["event"]["threadId"]})
    return session.stopped(marker, after)


def frame_at(session: RealLuaSession, stop: dict[str, Any], marker: str) -> dict[str, Any]:
    # Lua cannot always name a callback entered through native pcall. Its actual suspended call
    # site identifies the frame independently of a display name or any supplied frame identifier.
    line = source_line(session.source, marker)
    frames = [frame for frame in stop["stack"]["stackFrames"] if frame.get("line") == line
              and physical_source(session.project, frame.get("source", {})) == session.script.resolve()]
    require(len(frames) == 1, f"real stopped call stack omitted its unique {marker} call site")
    return {**stop, "top": frames[0]}


def run_scenario(session: RealLuaSession, identity: str) -> dict[str, Any]:
    capabilities = session.initialize()
    if identity == "handshake.initialize":
        require(capabilities.get("supportsConfigurationDoneRequest") is True,
                "real adapter did not negotiate configurationDone")
        return {"capabilities": capabilities, "authenticatedProcessId": session.dap.bootstrap["processId"]}
    session.attach()
    oracle = fixture_oracle(session.source)

    if identity == "handshake.attach_and_configuration_done":
        stop = stopped_at(session, "R7_ON_CREATE")
        return {"onCreateStop": stop["top"], "actualThread": stop["event"]["threadId"]}

    if identity in ("threads.single_lua_thread", "execution.pause_and_stopped_event"):
        session.start()
        session.await_play_started()
        threads = session.dap.request("threads").get("threads")
        require(isinstance(threads, list) and len(threads) == 1 and threads[0].get("id", 0) > 0,
                "single-script fixture did not expose its one actual main Lua thread")
        if identity.startswith("threads."):
            return {"threads": threads}
        after = session.dap.event_cursor
        session.dap.request("pause", {"threadId": threads[0]["id"]})
        stop = session.stopped(after=after, reason="pause")
        require(stop["event"]["threadId"] == threads[0]["id"], "pause stopped a different actual Lua thread")
        return {"stop": stop}

    if identity == "breakpoints.line":
        stop = stopped_at(session, "R7_ON_CREATE")
        return {"actualOnCreateFrame": stop["top"], "reason": stop["event"].get("reason")}

    if identity == "breakpoints.multiple_lines":
        markers = ["R7_ENTRY", "R7_LOCALS", "R7_AFTER_CALL"]
        submitted = session.breakpoints(markers)
        session.start()
        observed = []
        after = 0
        for marker in markers:
            stop = session.stopped(marker, after)
            observed.append(stop["top"])
            if marker != markers[-1]:
                after = session.dap.event_cursor
                session.dap.request("continue", {"threadId": stop["event"]["threadId"]})
        return {"submitted": submitted, "actualSequentialStops": observed}

    if identity == "breakpoints.conditional":
        submitted = session.breakpoints(["R7_ENTRY", "R7_LOCALS"], [
            {"condition": "self.ticks < 0"}, {"condition": f"speed == {oracle['speed']}"}])
        session.start()
        stop = session.stopped("R7_LOCALS")
        require(numeric(session.evaluate("speed", stop)) == oracle["speed"], "condition did not observe the live local")
        rejected_line = source_line(session.source, "R7_ENTRY")
        false_verified = session.dap.wait_event("breakpoint", predicate=lambda value:
            value.get("reason") == "changed" and value.get("breakpoint", {}).get("verified") is True
            and value.get("breakpoint", {}).get("line") == rejected_line)["breakpoint"]
        require(false_verified.get("id") not in stop["event"].get("hitBreakpointIds", []),
                "false condition contributed to the actual stop")
        return {"actualStop": stop, "conditionValue": session.evaluate("speed", stop)}

    if identity == "breakpoints.function":
        result = session.dap.request("setFunctionBreakpoints", {"breakpoints": [{"name": "Leaf"}]})
        require(isinstance(result.get("breakpoints"), list), "real function breakpoint reply is missing")
        session.start()
        stop = session.stopped("R7_LEAF_ENTRY")
        require("Leaf" in stop["top"].get("name", ""), "function breakpoint did not enter the actual Leaf function")
        return {"functionStop": stop["top"], "breakpoints": result["breakpoints"]}

    if identity == "stepping.step_in":
        stop = stopped_at(session, "R7_STEP_IN")
        session.breakpoints([])
        after = session.dap.event_cursor
        session.dap.request("stepIn", {"threadId": stop["event"]["threadId"]})
        entered = session.stopped("R7_LEAF_ENTRY", after)
        return {"before": stop["top"], "after": entered["top"]}

    stop = stopped_at(session)
    thread = stop["event"]["threadId"]

    if identity == "stack.stack_trace_while_stopped":
        frames = stop["stack"]["stackFrames"]
        locations = ("R7_LOCALS", "R7_STEP_IN", "R7_CALL_MIDDLE")
        require(all(any(item.get("line") == source_line(session.source, marker) for item in frames) for marker in locations),
                "real nested Lua call stack omitted an actually executing fixture call site")
        for item in frames:
            source = item.get("source")
            if source and source.get("path"):
                require(physical_source(session.project, source) == session.script.resolve(), "foreign source in fixture stack")
                require(type(item.get("line")) is int and 0 < item["line"] <= len(session.source.splitlines()),
                        "real stack source line is outside the actual loaded file")
        return {"actualStack": stop["stack"]}

    if identity == "stack.stack_trace_paging":
        whole = stop["stack"]
        require(len(whole["stackFrames"]) > 1, "real nested fixture stack has no second frame")
        page = session.dap.request("stackTrace", {"threadId": thread, "startFrame": 1, "levels": 1})
        def location(frame: dict[str, Any]) -> dict[str, Any]:
            return {key: value for key, value in frame.items() if key != "id"}
        actual = page.get("stackFrames")
        require(isinstance(actual, list) and all(type(frame.get("id")) is int and frame["id"] > 0 for frame in actual),
                "actual stack page omitted valid frame handles")
        require(page.get("totalFrames") == whole.get("totalFrames") and
                [location(frame) for frame in actual] == [location(frame) for frame in whole["stackFrames"][1:2]],
                "real stackTrace paging differs from the same actual stop's full stack")
        return {"whole": whole, "actualPage": page}

    if identity == "source.loaded_source_retrieval":
        source = stop["top"]["source"]
        reply = session.dap.request("source", {"source": source, "sourceReference": source.get("sourceReference", 0)})
        require(reply.get("content") == session.source, "real source response differs from the loaded physical Lua bytes")
        return {"actualSource": source, "sourceBytes": len(reply["content"].encode()), "mimeType": reply.get("mimeType")}

    if identity in ("execution.continue_and_continued_event", "lifecycle.owned_play_stop", "lifecycle.disconnect"):
        session.breakpoints([])
        after = session.dap.event_cursor
        if identity == "execution.continue_and_continued_event":
            session.dap.request("continue", {"threadId": thread})
            continued = session.dap.wait_event("continued", after)
            require(continued.get("threadId") == thread, "continued event changed the real stopped thread")
            return {"actualContinued": continued}
        detach = session.detach()
        session.await_play_started()
        if identity == "lifecycle.disconnect":
            state = session.engine.request("play.state", {}).get("result")
            require(isinstance(state, dict) and state.get("mode") == "Play", "detach stopped the external editor Play session")
            require(state.get("operationId") == session.operation_id and state.get("phase") == "playing" and
                    state.get("playGeneration") == session.play_generation, "detach changed actual Play operation identity")
            return {"actualPlayAfterDetach": state, "actualDetachAcknowledgement": detach}
        return {**session.stop_owned(), "actualDetachAcknowledgement": detach}

    if identity == "breakpoints.hit_condition":
        entry = continue_to(session, stop, "R7_ENTRY")
        initial_ticks = numeric(session.evaluate("self.ticks", entry))
        hits = 3
        session.breakpoints(["R7_ENTRY"], [{"hitCondition": str(hits)}])
        after = session.dap.event_cursor
        session.dap.request("continue", {"threadId": thread})
        third = session.stopped("R7_ENTRY", after)
        actual = numeric(session.evaluate("self.ticks", third))
        require(actual == initial_ticks + hits, "hit condition did not count actual callback visits")
        return {"initialTicks": initial_ticks, "actualTicks": actual, "requestedHits": hits}

    if identity == "breakpoints.logpoint":
        session.breakpoints(["R7_LOCALS", "R7_AFTER_CALL"], [{"logMessage": "R7_REAL_LOG speed={speed}"}, {}])
        after = session.dap.event_cursor
        session.dap.request("continue", {"threadId": thread})
        next_stop = session.stopped("R7_AFTER_CALL", after)
        # The first current LOCALS event has already passed. Continue to a new actual callback.
        after = session.dap.event_cursor
        session.dap.request("continue", {"threadId": thread})
        session.stopped("R7_AFTER_CALL", after)
        output = session.dap.wait_event("output", after,
            lambda value: f"R7_REAL_LOG speed={int(oracle['speed'])}" in value.get("output", ""))
        return {"actualOutput": output, "stopAfterLogpoint": next_stop["top"]}

    if identity == "breakpoints.pausepoint":
        session.breakpoints(["R7_ENTRY"], [{"mode": "pausepoint"}])
        after = session.dap.event_cursor
        session.dap.request("continue", {"threadId": thread})
        actual = session.stopped("R7_ENTRY", after)
        return {"actualPausepointStop": actual["top"], "reason": actual["event"].get("reason")}

    if identity == "breakpoints.exception":
        filters = capabilities.get("exceptionBreakpointFilters", [])
        require(any(item.get("filter") == "uncaught" for item in filters),
                "actual adapter omitted its protected-call uncaught Lua error filter")
        session.dap.request("setExceptionBreakpoints", {"filters": ["uncaught"]})
        session.evaluate("self.fail = true", frame_at(session, stop, "R7_CALL_MIDDLE"), "repl")
        session.breakpoints([])
        after = session.dap.event_cursor
        session.dap.request("continue", {"threadId": thread})
        actual = session.stopped("R7_EXCEPTION", after, "exception")
        return {"actualExceptionStop": actual}

    if identity in ("stepping.step_over", "stepping.step_out"):
        command, marker = ("next", "R7_STEP_OVER") if identity.endswith("over") else ("stepOut", "R7_STEP_OUT")
        session.breakpoints([])
        after = session.dap.event_cursor
        session.dap.request(command, {"threadId": thread})
        stepped = session.stopped(marker, after)
        return {"before": stop["top"], "after": stepped["top"]}

    if identity == "variables.scopes":
        scopes = session.scopes(stop)
        scoped(session, stop, "local")
        scoped(session, stop, "upvalue")
        return {"actualScopes": scopes}

    if identity == "variables.locals":
        _, locals_ = scoped(session, stop, "local")
        require(numeric(by_name(locals_, "speed")) == oracle["speed"] and
                numeric(by_name(locals_, "dt")) == oracle["dt"], "real locals differ from executed fixture constants")
        return {"actualLocals": locals_}

    if identity == "variables.upvalues":
        _, values = scoped(session, stop, "upvalue")
        require(numeric(by_name(values, "captured")) == oracle["captured"], "real closure upvalue is incorrect")
        return {"actualUpvalues": values}

    if identity == "variables.tables":
        _, values = scoped(session, stop, "local")
        table = by_name(values, "inventory")
        children = session.variables(table["variablesReference"])
        nested = by_name(children, "nested")
        require(numeric(by_name(session.variables(nested["variablesReference"]), "answer")) == oracle["answer"],
                "real nested table value differs from executed source")
        cycle = by_name(values, "cycle")
        self_entry = by_name(session.variables(cycle["variablesReference"]), "self")
        require(self_entry.get("variablesReference", 0) > 0, "real cyclic table expansion lost the Lua table")
        return {"actualInventory": children, "actualCycleSelf": self_entry}

    if identity == "variables.set_variable":
        reference, _ = scoped(session, stop, "local")
        replacement = oracle["speed"] + 15
        session.dap.request("setVariable", {"variablesReference": reference, "name": "speed", "value": str(replacement)})
        require(numeric(session.evaluate("speed", stop)) == replacement, "setVariable did not mutate the actual stopped local")
        completed = continue_to(session, stop, "R7_AFTER_CALL")
        result = numeric(session.evaluate("result", completed))
        require(math.isclose(result, replacement * oracle["dt"] + oracle["captured"] + 1),
                "resumed Lua execution did not use the mutated local")
        return {"mutatedSpeed": replacement, "actualResumedResult": result}

    if identity == "evaluate.watch":
        value = session.evaluate("speed * dt + captured", stop)
        require(math.isclose(numeric(value), oracle["speed"] * oracle["dt"] + oracle["captured"]),
                "watch evaluation differs from the independent source-derived arithmetic oracle")
        return {"actualWatch": value}

    if identity == "evaluate.repl":
        session.evaluate("speed = speed + 1", stop, "repl")
        result = session.evaluate("speed", stop)
        require(numeric(result) == oracle["speed"] + 1, "REPL assignment did not mutate the actual stopped Lua local")
        return {"actualReplMutation": result}

    if identity == "coroutines.multi_coroutine_threads":
        session.evaluate("self.spawn = true", frame_at(session, stop, "R7_CALL_MIDDLE"), "repl")
        session.breakpoints(["R7_CHILD_COROUTINE"])
        after = session.dap.event_cursor
        session.dap.request("continue", {"threadId": thread})
        child = session.stopped("R7_CHILD_COROUTINE", after)
        child_thread = child["event"]["threadId"]
        require(child_thread != thread, "new actual coroutine reused the parent Lua thread identity")
        created = session.dap.wait_event("thread", after,
            lambda value: value.get("reason") == "started" and value.get("threadId") == child_thread)
        threads = session.dap.request("threads").get("threads")
        require(isinstance(threads, list) and any(item.get("id") == child_thread for item in threads),
                "threads did not enumerate the newly stopped actual coroutine")
        require(numeric(session.evaluate("childValue", child)) == oracle["childValue"], "coroutine stack uses invented locals")
        return {"actualNewThreadEvent": created, "actualThreads": threads, "actualCoroutineFrame": child["top"]}

    if identity == "lifecycle.terminate":
        after = session.dap.event_cursor
        session.dap.request("terminate", {"restart": False})
        ended = session.dap.wait_event("terminated", after)
        return {"actualVmTerminated": ended, "editorProcessId": session.dap.bootstrap["processId"]}

    raise AssertionError(f"real conformance handler missing for catalogue identity {identity}")
