"""Small independently bounded offline test groups; never changes a deadline."""
import unittest


PUBLISH = {"test_complete_publication_and_replay_in_fresh_workers",
           "test_lost_commit_receipt_never_aborts_and_fresh_worker_recovers"}
DEADLINE = {"test_full_worker_deadline_after_commit_leaves_unknown_then_recovers"}


def worker_suite(directory, group):
    if group not in ("preflight", "publish", "deadline", "refusal", "initialize", "quota", "quota-stop", "credentials", "identity", "object-deadline", "live-stages", "permission-stages", "permission-verify", "reply-loss", "reply-loss-stop", "reply-loss-unit", "reply-loss-final", "terminal-unit", "terminal-commit", "terminal-read", "terminal-stop"):
        raise ValueError("invalid_worker_test_group")
    all_tests = unittest.defaultTestLoader.discover(directory, pattern="test_*.py")
    def cases(suite):
        for test in suite:
            if isinstance(test, unittest.TestCase):
                yield test
            else:
                yield from cases(test)
    def selected(test):
        module = test.__class__.__module__
        if module == "test_terminal_unit":return group == "terminal-unit"
        if group == "terminal-unit":return False
        if module == "test_terminal_worker":
            return group == ("terminal-read" if test._testMethodName.startswith("test_existing_") else
                             "terminal-stop" if test._testMethodName.startswith("test_lost_") else "terminal-commit")
        if group in ("terminal-read", "terminal-stop", "terminal-commit"):return False
        if module == "test_reply_loss_unit":
            return group == "reply-loss-unit"
        if group == "reply-loss-unit":
            return False
        if module == "test_reply_loss_worker":
            return group == ("reply-loss-stop" if test._testMethodName.startswith("test_wire_loss") else
                             "reply-loss-final" if test._testMethodName.startswith("test_terminal_") else "reply-loss")
        if group in ("reply-loss", "reply-loss-stop", "reply-loss-final"):
            return False
        if module == "test_permission_verify_worker":
            return group == "permission-verify"
        if group == "permission-verify":
            return False
        if module == "test_permission_worker":
            return group == "permission-stages"
        if group == "permission-stages":
            return False
        if module == "test_live_trial_worker":
            return group == "live-stages"
        if group == "live-stages":
            return False
        if module == "test_credentials_windows":
            return group == "credentials"
        if module == "test_identity_worker":
            return group == "identity"
        if module == "test_object_deadline":
            return group == "object-deadline"
        if group in ("credentials", "identity", "object-deadline"):
            return False
        if module == "test_initialize_worker":
            return group == "initialize"
        if module == "test_trial_worker":
            full_path = test._testMethodName == "test_initialize_publish_and_lost_commit_recovery_share_budget"
            return group == ("quota" if full_path else "quota-stop")
        if group in ("initialize", "quota", "quota-stop"):
            return False
        preflight = test.__class__.__module__ == "test_preflight"
        if group == "preflight":
            return preflight
        if preflight:
            return False
        if group == "publish":
            return test._testMethodName in PUBLISH
        if group == "deadline":
            return test._testMethodName in DEADLINE
        return test._testMethodName not in PUBLISH | DEADLINE
    selected_suite = unittest.TestSuite(test for test in cases(all_tests) if selected(test))
    if selected_suite.countTestCases() == 0:
        raise ValueError("empty_worker_test_group")
    return selected_suite
