# Copy to <git>/config.mak to build Git with ficc (Phase 23).
#
# `CC` is the gcc-style wrapper; it compiles with ficc and links with gcc
# -no-pie (ficc objects carry R_X86_64_32S data relocations). Feature switches
# keep the dependency surface small; HAVE_ALLOCA_H is cleared so xalloca falls
# back to xmalloc (ficc has no alloca).

CC = /home/tfic/projects/ficc/test/git-cc.sh
CFLAGS = -O0
NO_RUST = 1
NO_OPENSSL = 1
NO_CURL = 1
NO_EXPAT = 1
NO_GETTEXT = 1
NO_ICONV = 1
NO_SYSLOG = 1
NO_PERL = 1
NO_PYTHON = 1
NO_TCLTK = 1
HAVE_ALLOCA_H =
