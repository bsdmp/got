.PATH:${.CURDIR}/../../lib

.include "../../got-version.mk"

PROG=		got-read-gotconfig
SRCS=		got-read-gotconfig.c error.c inflate.c object_parse.c \
		path.c privsep.c sha1.c parse.y pollfd.c

CPPFLAGS = -I${.CURDIR}/../../include -I${.CURDIR}/../../lib -I${.CURDIR}

.if defined(PROFILE)
LDADD = -lutil_p -lz_p
.else
LDADD = -lutil -lz
.endif

DPADD = ${LIBZ} ${LIBUTIL}

.include <bsd.prog.mk>
