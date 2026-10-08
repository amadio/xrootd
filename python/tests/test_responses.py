import errno

import pytest

from XRootD.client.responses import XRootDStatus, XRootDNotFoundError, \
  XRootDAuthorizationError, \
  XRootDTimeoutError, XRootDChecksumError, XRootDOperationError, \
  XRootDAlreadyExistsError, XRootDQuotaError, XRootDTemporaryError, \
  XRootDUnsupportedError, \
  ChecksumInfo, raise_on_error, raise_as_oserror, parse_checksum, \
  checksum_query_path


def status(code, ok=False, shellcode=0, message='error', errno=0):
  return XRootDStatus({
    'message': message,
    'ok': ok,
    'error': not ok,
    'fatal': False,
    'status': 0 if ok else 1,
    'code': code,
    'shellcode': shellcode,
    'errno': errno,
  })


def test_status_error_name_and_exceptions():
  assert status(XRootDStatus.errNotFound).error_name == 'errNotFound'
  assert status(XRootDStatus.errPipelineFailed).error_name == \
    'errPipelineFailed'
  assert status(XRootDStatus.errTlsError).error_name == 'errTlsError'
  assert status(999).error_name is None
  assert isinstance(status(XRootDStatus.errNotFound).exception(),
                    XRootDNotFoundError)
  assert isinstance(status(XRootDStatus.errAuthFailed).exception(),
                    XRootDAuthorizationError)
  assert isinstance(status(XRootDStatus.errSocketTimeout).exception(),
                    XRootDTimeoutError)
  assert isinstance(status(XRootDStatus.errCheckSumError).exception(),
                    XRootDChecksumError)
  assert isinstance(status(XRootDStatus.errUnknown).exception(),
                    XRootDOperationError)


@pytest.mark.parametrize('errno, exception_type', [
  (3011, XRootDNotFoundError),
  (3010, XRootDAuthorizationError),
  (3030, XRootDAuthorizationError),
  (3034, XRootDTimeoutError),
  (3035, XRootDTimeoutError),
  (3019, XRootDChecksumError),
  (3018, XRootDAlreadyExistsError),
  (3032, XRootDAlreadyExistsError),
  (3021, XRootDQuotaError),
  (3003, XRootDTemporaryError),
  (3024, XRootDTemporaryError),
  (3013, XRootDUnsupportedError),
  (3012, XRootDOperationError),
])
def test_server_error_exceptions(errno, exception_type):
  error = status(XRootDStatus.errErrorResponse, shellcode=54, errno=errno)
  assert isinstance(error.exception(), exception_type)


def test_raise_on_error():
  ok = status(XRootDStatus.errNone, ok=True, message='ok')
  assert raise_on_error(ok) is ok
  assert isinstance(raise_on_error(ok.__dict__), XRootDStatus)

  with pytest.raises(XRootDNotFoundError) as excinfo:
    status(XRootDStatus.errNotFound).raise_on_error()
  assert excinfo.value.status.code == XRootDStatus.errNotFound


def test_checksum_info():
  checksum = ChecksumInfo('adler32 deadbeef\n')
  assert checksum.algorithm == 'adler32'
  assert checksum.value == 'deadbeef'

  checksum = ChecksumInfo(b'adler32 deadbeef\0')
  assert checksum.algorithm == 'adler32'
  assert checksum.value == 'deadbeef'

  checksum = ChecksumInfo('md5 abc123\0')
  assert checksum.algorithm == 'md5'
  assert checksum.value == 'abc123'


@pytest.mark.parametrize('response', ['', 'adler32', 'adler32\0'])
def test_checksum_info_rejects_invalid_response(response):
  with pytest.raises(ValueError, match='Invalid checksum response'):
    ChecksumInfo(response)


@pytest.mark.parametrize('response', [b'\xff', b'adler32', b'',
                                     b'adler32 01234567 trailing'])
def test_checksum_parser_reports_protocol_errno(response):
  with pytest.raises(OSError) as caught:
    parse_checksum(response, 'ADLER32')
  assert caught.value.errno == errno.EPROTO


def test_checksum_parser_reports_algorithm_mismatch_errno():
  with pytest.raises(OSError) as caught:
    parse_checksum(b'adler32 01234567', 'MD5')
  assert caught.value.errno == errno.EINPROGRESS
  assert parse_checksum(b'adler32 01234567', 'ADLER32') == \
    ('adler32', '01234567')


def test_checksum_query_normalizes_algorithm_without_changing_other_params():
  assert checksum_query_path('/file?x=a%2Fb&x=2&cks.type=MD5', 'ADLER32') == \
    '/file?x=a%2Fb&x=2&cks.type=adler32'


@pytest.mark.parametrize('code, expected', [
  (XRootDStatus.errNotSupported, errno.ENOTSUP),
  (XRootDStatus.errQueryNotSupported, errno.ENOTSUP),
  (XRootDStatus.errNotImplemented, errno.ENOTSUP),
  (XRootDStatus.errInvalidArgs, errno.EINVAL),
])
def test_query_failure_reports_os_errno(code, expected):
  native = status(code)
  with pytest.raises(OSError) as caught:
    raise_as_oserror(native, '/file')
  assert caught.value.errno == expected
  assert caught.value.xrootd_status is native
