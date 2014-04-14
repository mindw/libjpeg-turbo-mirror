This directory contains copies of the IJG source code, for reference purposes.

The source has been modified in the following ways:
-- The jpeg-6b build system has been updated to use autotools.
-- The configure.ac files have been modified to support older versions of
autotools and to prevent autoheader from clobbering jconfig.cfg.
-- The TurboJPEG API and tools have been back-ported from libjpeg-turbo, for
the purposes of benchmarking.  The YUV planar functions in the API do not
generally work with jpeg-7 and later, because they rely on the internal
behavior of jpeg-6b.
