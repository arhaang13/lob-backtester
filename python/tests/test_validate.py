from conftest import requires_amzn
from lob.validate import validate


@requires_amzn
def test_amzn_snapshot_resync_is_exact():
    rep = validate("AMZN")
    assert rep.modes["snapshot_resync"].exact_pct == 100.0
    # message-only inference recovers most of L1 but cannot be exact for level-k data
    assert rep.modes["priority_purge"].l1_pct > rep.modes["message_only"].l1_pct
