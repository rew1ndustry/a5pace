"""asp5 store helpers: state is a fold of the log (see docs/DATA.md)."""
import copy, json, sqlite3

def fold(db, upto=None):
    """Return {take_id: state dict} as of log point `upto` (default: all).
    A revert{to:N} event replaces the state with the fold at N."""
    cur = db.execute("select seq,type,args_json from log where seq<=? order by seq",
                     (upto if upto is not None else 1 << 62,))
    hist = []                       # (seq, state) after each event
    state = {}
    for seq, typ, a in cur:
        a = json.loads(a)
        if typ == "revert":
            state = copy.deepcopy(next((s for q, s in reversed(hist)
                                if q <= a["to"]), {}))
        elif typ == "take_state":
            state = dict(state); state[a["take"]] = a["state"]
        hist.append((seq, state))
    return state
