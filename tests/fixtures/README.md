# WC04 descriptor fixture

`wc04-configuration.bin` is the 777-byte configuration descriptor extracted
from ZodiaKGalXy's supplied 0.1.0 session log, received 2026-10-06. All five
descriptor dumps in that log are byte-identical. The tester's full session
log is not distributed with the test fixture.

This is real descriptor evidence, not image payload evidence. The artificial
descriptor and packet fixtures in `test_uvc.c` remain useful for malformed-input
and framing tests. The 12-byte headers in the diagnostic test are synthetic;
the original hardware log contains counts but no packet bytes.
