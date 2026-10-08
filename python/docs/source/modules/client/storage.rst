StorageClient
=============

.. autoclass:: XRootD.client.StorageClient
   :members:

``StorageClient`` provides thread-safe application operations with one default
operation budget and object-scoped authentication.  A client created with
``from_environment`` owns and closes its authentication context::

  from XRootD import client

  with client.StorageClient.from_environment(timeout=300) as storage:
      storage.probe('davs://storage.example/rucio', timeout=10)
      info = storage.info(
          'davs://storage.example/rucio/file',
          checksum_algorithms=('adler32', 'md5'),
          require_checksum=True)

.. autoclass:: XRootD.client.StorageInfo
   :members:

``timeout`` is a finite number in ``(0, 65535]`` seconds. ``None`` leaves native
request defaults in effect. Metadata, parent creation and checksum negotiation
share one budget, with native request timeouts rounded up to whole seconds.
Copies check the budget after preparation and request cancellation through the
native progress callback when it expires. Cancellation is cooperative: blocked
requests and cleanup can outlive the budget, which is not a hard kill deadline.

``put`` requires ``create_parents=False`` for query-bearing destinations, so
signed file URLs are not silently reused or stripped for parent requests.
``move`` is confined to one endpoint and rejects query-bearing operands; create
parents explicitly when using resource-specific authorization parameters.

``FileSystem.checksum`` retains the raising tuple API from the Pythonic helper
series. ``FileSystem.checksum_info`` is the additive status/structured-response
API used for application checksum negotiation. Existing filesystem helpers,
streams, asyncio and optional fsspec retain their public contracts.
