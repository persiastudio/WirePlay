"""need_credits: the A/B switch for the NEED credit pool."""
import quicktime_backend as qb


def test_default_is_the_current_experiment():
    assert qb.need_credits({}) == 9


def test_env_overrides_and_is_clamped():
    assert qb.need_credits({"WIREPLAY_NEED_CREDITS": "1"}) == 1
    assert qb.need_credits({"WIREPLAY_NEED_CREDITS": "2"}) == 2
    assert qb.need_credits({"WIREPLAY_NEED_CREDITS": "0"}) == 1     # at least one
    assert qb.need_credits({"WIREPLAY_NEED_CREDITS": "500"}) == 32  # bounded


def test_garbage_falls_back_to_the_default():
    assert qb.need_credits({"WIREPLAY_NEED_CREDITS": "many"}) == 9
    assert qb.need_credits({"WIREPLAY_NEED_CREDITS": ""}) == 9
