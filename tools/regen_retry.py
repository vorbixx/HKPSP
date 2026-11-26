"""Retry narrowly identified transient converter process launch failures."""
import errno
import re
import time


_STATUS_DLL_INIT_FAILED = 0xC0000142
_STATUS_DLL_INIT_FAILED_TEXT = re.compile(
    r'(?<![\w])(?:0xc0000142|3221225794|-1073741502)(?!\w)', re.IGNORECASE)


def retryable_worker_log(result, text):
    """Identify retryable converter failures reported in a worker log."""
    returncode = getattr(result, 'returncode', None)
    if returncode in (None, 0):
        return False
    for line in text.splitlines():
        lowered = line.lower()
        if ('permissionerror' in lowered and ('winerror 5' in lowered or 'errno 13' in lowered)
                and '.t3x' in lowered):
            return True
        if 'calledprocesserror' in lowered and _STATUS_DLL_INIT_FAILED_TEXT.search(line):
            return True
    return False


def _is_t3x_command(args):
    return any('.t3x' in str(argument).lower() for argument in args)


def _is_access_denied(error):
    return getattr(error, 'winerror', None) == 5 or error.errno in (errno.EACCES, errno.EPERM)


def _is_retryable_error(error, args):
    return (getattr(error, 'winerror', None) == 1114 or
            (_is_t3x_command(args) and _is_access_denied(error)))


def _is_retryable_result(result):
    return getattr(result, 'returncode', None) is not None and (
        result.returncode & 0xFFFFFFFF) == _STATUS_DLL_INIT_FAILED


def run_with_retries(runner, args, *, should_retry_result=None, retries=3,
                     backoff=0.25, sleeper=time.sleep, **kwargs):
    """Call a subprocess-compatible ``runner`` with bounded transient retries.

    ``retries`` is the number of additional attempts after the initial call.
    ``should_retry_result`` may classify failures whose details are outside the
    result, such as a redirected worker log inspected by a closure.
    The original args and kwargs are passed unchanged on every attempt; the
    successful result is returned and the final failure is propagated intact.
    """
    if retries < 0:
        raise ValueError('retries must be non-negative')
    for attempt in range(retries + 1):
        try:
            result = runner(args, **kwargs)
        except OSError as error:
            retryable = _is_retryable_error(error, args)
            if not retryable or attempt == retries:
                raise
        else:
            retryable = (_is_retryable_result(result) or
                         (should_retry_result is not None and should_retry_result(result)))
            if not retryable or attempt == retries:
                return result
        sleeper(backoff * (2 ** attempt))
