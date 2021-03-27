# Process this file with autoconf to produce a configure script.
#
# This file is part of DisOrder.
# Copyright (C) 2004-2014 Richard Kettlewell
# Portions copyright (C) 2007 Ross Younger
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

AC_INIT([disorder], [5.2~wry1], [crazyscot@gmail.com])
AC_CONFIG_AUX_DIR([config.aux])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_SRCDIR([server/disorderd.c])
AM_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([m4])

# Find host type
AC_CANONICAL_HOST

# What we want to build
want_gtk=yes
want_python=yes
want_tests=yes
want_server=yes
want_gstdecode=whatever
want_cgi=yes

# APIs we want
want_alsa=yes
want_oss=yes
want_coreaudio=yes
want_pulseaudio=yes
want_gstreamer=yes

# By default we don't want gtk-osx.  But if you ask for --with-gtk-osx...
#
# Disobedience can be built to a native OS X application.  There are some
# caveats:
#  - you will still need Fink (or something) for other libraries
#  - if you wanted an application bundle you are out of luck
#  - the menu bar is still in the main window (not at the top of the screen)
#  - drag and drop doesn't work
#
# So it's still rather rough and ready.  But it does work...
#
# See:
#   http://www.gtk-osx.org/
#   http://live.gnome.org/GTK%2B/OSX/Framework
want_gtkosx=no

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_AWK
AC_SET_MAKE
if test "x$GCC" = xyes; then
  gcc_werror=-Werror
  case "$CC" in
  *-std=* )
    ;;
  * )
    CC="${CC} -std=gnu99"
    ;;
  esac
else
  AC_MSG_ERROR([GNU C is required to build this program])
  gcc_werror=""
fi

AC_ARG_WITH([alsa],
	    [AS_HELP_STRING([--without-alsa],
			    [do not build with ALSA support])],
	    [want_alsa=$withval])
AC_ARG_WITH([pulseaudio],
	    [AS_HELP_STRING([--without-pulseaudio],
			    [do not build with PulseAudio support])],
	    [want_pulseaudio=$withval])
AC_ARG_WITH([oss],
	    [AS_HELP_STRING([--without-oss],
			    [do not build with OSS support])],
	    [want_oss=$withval])
AC_ARG_WITH([coreaudio],
	    [AS_HELP_STRING([--without-coreaudio],
			    [do not build with Core Audio support])],
	    [want_coreaudio=$withval])
AC_ARG_WITH([tests],
	    [AS_HELP_STRING([--without-tests],
			    [do not build test suit])],
	    [want_tests=$withval])
AC_ARG_WITH([gtk-osx],
	    [AS_HELP_STRING([--with-gtk-osx],
			    [use native GTK+ (OS X only)])],
	    [want_gtkosx=$withval])
AC_ARG_WITH([gstreamer],
            [AS_HELP_STRING([--with-gstreamer], [select GStreamer version])],
            [want_gstreamer=$withval])

AC_MSG_CHECKING([for a known target platform])
case "$host" in
*empeg* )
  AC_MSG_RESULT([empeg car stereo])
  AC_DEFINE([EMPEG_HOST],[1],[define if host is an empeg car stereo])
  # work around broken toolchain
  AC_CHECK_LIB([gpg-error], [gpg_strerror])
  AC_CHECK_LIB([pthread], [pthread_create])
  want_server=no
  want_cgi=no
 ;;
*linux* | *Linux* )
  AC_MSG_RESULT([Linux])
  AC_DEFINE_UNQUOTED([PATH_PROC_MOUNTS],["/proc/mounts"],[path to kernel mount list])
  ;;
*-apple-darwin* )
  AC_MSG_RESULT([Mac OS X])
  # AudioHardwareGetProperty is deprecated in 10.6, but it's still _there_,
  # so we'll just turn off the warning.
  CC="$CC -Wno-deprecated-declarations"
  if test $want_coreaudio = yes; then
    COREAUDIO="-framework CoreFoundation -framework CoreServices -framework CoreAudio"
  fi
  browser=open
  AC_MSG_CHECKING([Mac OS X target version])
  # We honor MACOSX_DEPLOYMENT_TARGET in the environment, emulating gcc's
  # behaviour.  But we provide a command line option to override it and
  # we default to wide support instead of supporting only the build platform.
  #
  # Currently if you ask for 10.5 you will get a deprecation warning
  # when building the CoreAudio support code.  For the time being the
  # answer to this is "don't do that then".  If a good reason to ask
  # for a 10.5 deployment target emerges then this will be fixed.
  if test -z "$MACOSX_DEPLOYMENT_TARGET"; then
    MACOSX_DEPLOYMENT_TARGET=10.0
  fi
  AC_ARG_WITH([deployment-target],
              [AS_HELP_STRING([--with-deployment-target=TARGET],
                              [set target OS X version])],
              [MACOSX_DEPLOYMENT_TARGET=$withval])
  # Convert to desired format
  underscored=`echo $MACOSX_DEPLOYMENT_TARGET|sed 's/\./_/'`
  minver="MAC_OS_X_VERSION_$underscored"
  AC_MSG_RESULT([$minver])
  AC_DEFINE_UNQUOTED([MAC_OS_X_VERSION_MIN_REQUIRED], [$minver],
                     [define to minimum version of Mac OS X to support])

  if test $want_gtkosx = no; then
    # Fink's GTK+ is hopelessly broken
    PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-/sw/lib/pango-ft219/lib/pkgconfig}
    export PKG_CONFIG_PATH
  fi

  ;;
*-freebsd* )
  AC_MSG_RESULT([FreeBSD])
  # Ports install to /usr/local but the compiler stupidly doesn't look
  # there by default
  LDFLAGS="${LDFLAGS} -L/usr/local/lib"
  CPPFLAGS="${CPPFLAGS} -isystem /usr/local/include"
  # Look for a suitable version of libdb among the versions found in FreeBSD 7.0
  AC_CACHE_CHECK([looking for a libdb install],[rjk_cv_libdb],[
    rjk_cv_libdb="none"
    for db in db43 db44 db45 db47 db48; do
      if test -e /usr/local/lib/$db; then
        rjk_cv_libdb=$db
        break
      fi
    done
  ])
  if test $rjk_cv_libdb != none; then
    LDFLAGS="${LDFLAGS} -L/usr/local/lib/$rjk_cv_libdb"
    CPPFLAGS="${CPPFLAGS} -isystem /usr/local/include/$rjk_cv_libdb"
  fi
  # iconv needs bodging
  AC_SUBST(LIBICONV,[-liconv])
  ;;
* )
  AC_MSG_RESULT([unknown, winging it])
  ;;
esac
AC_SUBST([COREAUDIO])

AC_ARG_WITH([browser],
            [AS_HELP_STRING([--with-browser=BROWSER],
			    [use BROWSER to display HTML])],
            [browser=$withval])

AC_CACHE_CHECK([default HTML viewer],[rjk_cv_browser],[
  rjk_cv_browser=UNKNOWN
  for candidate in sensible-browser x-www-browser firefox mozilla chromium google-chrome konqueror netscape; do
    if type $candidate >/dev/null 2>&1; then
      rjk_cv_browser="$candidate"
      break
    fi
  done
])
if test -z "$browser"; then
  browser="$rjk_cv_browser"
fi
AC_DEFINE_UNQUOTED([BROWSER],["$browser"],[HTML viewer])

# Figure out what version of sox is installed
AC_CACHE_CHECK([sox version],[rjk_cv_soxver],[
  rjk_cv_soxver=UNKNOWN
  if sox --version > /dev/null 2>&1; then
    rjk_cv_soxver=`sox --version|$AWK '{ if(match($0, /[[0-9\.]]+/)) print substr($0,RSTART,RLENGTH)}'`
  else
    rjk_cv_soxver=`sox -h 2>&1|$AWK '/Version/ { if(match($0, /[[0-9\.]]+/)) print substr($0,RSTART,RLENGTH)}'`
  fi
])

# Decide what command line options to use
#
# sox version   endian     bits       deployed
# 12.17.9       -x         -[bwld]    debian etch, ubuntu dapper
# 14.0.0        -x/-[BL]   -[bwld]    ubuntu hardy
# 14.0.1        -x/-[BL]   -[1248]    debian lenny
# 14.2.0        -x/-[BL]   -[1248]    debian sid (at 2008-12)

AC_CACHE_CHECK([default sox generation],[rjk_cv_soxgen],[
  case $rjk_cv_soxver in
  [[0-9]].* | 1[[0123]].* )
    rjk_cv_soxgen=0
    ;;
  * )
    rjk_cv_soxgen=1
    ;;
  esac
])
AC_DEFINE_UNQUOTED([DEFAULT_SOX_GENERATION],[$rjk_cv_soxgen],
                   [default sox generation])

AC_ARG_WITH([server],
	    [AS_HELP_STRING([--without-server],
			    [do not build server])],
	    [want_server=$withval])
AC_ARG_WITH([cgi],
	    [AS_HELP_STRING([--without-cgi],
			    [do not build CGI])],
	    [want_cgi=$withval])
AC_ARG_WITH([gtk],
	    [AS_HELP_STRING([--without-gtk],
			    [do not build GTK+ client])],
	    [want_gtk=$withval])
AC_ARG_WITH([python],
	    [AS_HELP_STRING([--without-python],
			    [do not build Python support])],
	    [want_python=$withval])
AC_ARG_WITH([gstdecode],
            [AS_HELP_STRING([--with-gstdecode],
			    [require GStreamer-based decoder])],
            [want_gstdecode=$withval])

if test $want_server = no; then
  want_cgi=no
  want_gstdecode=no
fi

#
# OS          Document Root                  CGI directory
# ==          =============                  =============
# Debian      /var/www                       /usr/lib/cgi-bin
# Ubuntu      /var/www                       /usr/lib/cgi-bin
# Red Hat     /var/www/html                  /var/www/cgi-bin
#      or:    /home/httpd/html/              /home/httpd/cgi-bin
# Slackware   /var/www/htdocs                (unknown)
# SuSE        /srv/www/html                  /srv/www/cgi-bin
# Gentoo      /var/www/localhost/htdocs      /var/www/localhost/cgi-bin/
# FreeBSD     /usr/local/www/data            /usr/local/www/cgi-bin
#      or:    /usr/local/www/???/data        /usr/local/www/???/cgi-bin
# OS X        /Library/WebServer/Documents   /Library/WebServer/CGI-Executables
# Apache      /usr/local/apache/htdocs       (none)
#
# Sources:
#   http://wiki.linuxquestions.org/wiki/Apache
#   http://mapserver.gis.umn.edu/docs/howto/suse-linux
#   http://www.yolinux.com/TUTORIALS/LinuxTutorialWebSiteConfig.html
#   http://sources.gentoo.org/viewcvs.py/apache/trunk/dist/2.2/conf/vhosts.d/default_vhost.include?rev=198&view=log
#   http://httpd.apache.org/docs/
#
if test $want_cgi = yes; then
  if test -z "$httpdir"; then
    AC_CACHE_CHECK([for httpd document root],[rjk_cv_httpdir],[
      rjk_cv_httpdir="/usr/local/apache/htdocs"
      for dir in /var/www/html \
                 /var/www/htdocs \
                 /var/www/localhost/htdocs \
                 /var/www \
                 /home/httpd/html \
                 /srv/www/html \
                 /usr/local/www/data \
                 /usr/local/www/*/data \
                 /Library/WebServer/Documents \
                 /usr/local/apache/htdocs; do
        if test -d "$dir"; then
          rjk_cv_httpdir="$dir"
        fi
      done
    ])
    httpdir="$rjk_cv_httpdir"
  fi
  if test ! -z "$cgidir"; then
    # This is a bit harsh but should stop any disasters
    AC_MSG_ERROR([cgidir has been renamed to cgiexecdir])
  fi
  if test -z "$cgiexecdir"; then
    AC_CACHE_CHECK([for CGI directory],[rjk_cv_cgiexecdir],[
      rjk_cv_cgiexecdir="/usr/lib/cgi-bin"
      for dir in /usr/lib/cgi-bin \
                 /Library/WebServer/CGI-Executables \
                 /srv/www/cgi-bin \
                 /var/www/cgi-bin \
                 /home/httpd/cgi-bin \
                 /var/www/localhost/cgi-bin \
                 /usr/local/lib/cgi-bin \
                 /usr/local/www/cgi-bin \
                 /usr/local/www/*/cgi-bin; do
        if test -d "$dir"; then
          rjk_cv_cgiexecdir="$dir"
          break
        fi
      done
    ])
    cgiexecdir="$rjk_cv_cgiexecdir"
  fi
fi
AC_ARG_VAR([cgiexecdir], [location of cgi-bin directory, e.g. /usr/lib/cgi-bin])
AC_ARG_VAR([httpdir], [location of http document root, e.g. /var/www/htdocs])
if test -z "$pkghttpdir"; then
  pkghttpdir='$(httpdir)/disorder'
fi
AC_SUBST([pkghttpdir])

if test -z "$dochtmldir"; then
  dochtmldir='$(docdir)/html'
fi
AC_SUBST([dochtmldir])

subdirs="scripts common lib"
if test $want_tests = yes; then
  subdirs="${subdirs} libtests"
fi
subdirs="${subdirs} clients doc examples debian"

if test $want_server = yes; then
  subdirs="${subdirs} server plugins sounds"
fi
if test $want_cgi = yes; then
  subdirs="${subdirs} cgi templates images"
fi
if test $want_gtk = yes; then
  subdirs="${subdirs} disobedience"
  if test $want_server = no; then
    subdirs="${subdirs} images"
  fi
fi
if test $want_tests = yes && test $want_python = yes; then
  AM_PATH_PYTHON([2.5])
  subdirs="${subdirs} python tests"
fi
AC_SUBST([subdirs])

# libtool config
AC_LIBTOOL_DLOPEN
AC_DISABLE_STATIC

AC_PROG_LIBTOOL

AC_CACHE_CHECK([for GNU sed],[rjk_cv_gnused],[
  rjk_cv_gnused="not found"
  for candidate in sed gsed; do
    if $candidate --version >/dev/null 2>&1; then
      rjk_cv_gnused=$candidate 
    fi
  done
])
GNUSED="${GNUSED:-$rjk_cv_gnused}"
if test "$GNUSED" = "not found"; then
  AC_MSG_ERROR([GNU sed is required to build this program])
fi
AC_SUBST([GNUSED])

AC_CHECK_PROGS([GROG],[grog])
AM_CONDITIONAL([GROG],[test "x$GROG" != xnone])

missing_libraries=""
missing_headers=""
missing_functions=""

AC_DEFINE(_GNU_SOURCE, 1, [required for e.g. strsignal])

AC_PATH_PROG([SENDMAIL],[sendmail],[none],[$PATH:/usr/sbin:/usr/lib])

# Macs might have libraries under fink's root
AC_PATH_PROG([FINK],[fink],[none],[$PATH:/sw/bin])
if test "x$FINK" != xnone; then
  # Find Fink prefix
  AC_CACHE_CHECK([fink install directory],[rjk_cv_finkprefix],[
    rjk_cv_finkprefix="`echo "$FINK" | sed 's,/bin/fink$,,'`"
  ])
  # Add include and link paths
  finkdir="${rjk_cv_finkprefix}"
  finkbindir="${rjk_cv_finkprefix}/bin"
  CPPFLAGS="${CPPFLAGS} -isystem ${rjk_cv_finkprefix}/include/gc -isystem ${rjk_cv_finkprefix}/include"
  if test $want_server = yes; then
    CPPFLAGS="${CPPFLAGS} -isystem ${rjk_cv_finkprefix}/include/db4"
  fi
  LDFLAGS="${LDFLAGS} -L${rjk_cv_finkprefix}/lib"
  # Distinguish 32- and 64-bit fink
  AC_CACHE_CHECK([whether Fink is 32-bit or 64-bit],[rjk_cv_finkbits],[
    odata=`otool -v -h $finkbindir/dpkg`
    case "$odata" in
    *X86_64* )
      rjk_cv_finkbits=64
      ;;
    *I386* )
      rjk_cv_finkbits=32
      ;;
    * )
      rjk_cv_finkbits=unknown
      ;;
    esac
  ])
  # Match Fink so we can use its libraries
  case $rjk_cv_finkbits in
  32 | 64 )
    CC="$CC -m$rjk_cv_finkbits"
    ;;
  esac
else
  finkbindir=""
fi
AC_SUBST([finkdir])
AC_SUBST([finkbindir])

# Checks for packages.
case $want_gstdecode,$want_gstreamer in
  yes,no) AC_MSG_ERROR([gstdecode requires GStreamer]) ;;
  whatever,no) want_gstdecode=no ;;
  no,*) want_gstreamer=no ;;
  *,yes) want_gstreamer="1.0 0.10" ;;
esac

AH_TEMPLATE([HAVE_GSTREAMER_0_10],
    [Define if building against GStreamer 0.10])
AH_TEMPLATE([HAVE_GSTREAMER_1_0],
    [Define if building against GStreamer 1.0])

mdw_gstreamer_version=nil
for i in $want_gstreamer; do
  case $i in
    0.10) v=0.10 V=0_10 ;;
    1.0) v=1.0 V=1_0 ;;
    *) AC_MSG_ERROR([unrecognized GStreamer version]) ;;
  esac
  PKG_CHECK_MODULES([GSTREAMER],
      [gstreamer-$v gstreamer-app-$v gstreamer-audio-$v gstreamer-pbutils-$v],
      [have_gstreamer=$v], [have_gstreamer=no])
  case $have_gstreamer in no) continue ;; esac
  AC_DEFINE_UNQUOTED([HAVE_GSTREAMER_$V], [1])
  mdw_gstreamer_version=$v
  break
done

case $want_gstdecode,$have_gstreamer,$have_gst_plugins_base in
  whatever,no,* | whatever,*,no)
    want_gstdecode=no
    ;;
  yes,no,* | yes,*,no)
    case $have_gstreamer in
      no) missing_libraries="$missing_libraries gstreamer" ;;
    esac
    ;;
  *)
    want_gstdecode=yes
    ;;
esac

mdw_SAVE_CFLAGS=$CFLAGS
mdw_SAVE_LIBS=$LIBS
CFLAGS="$CFLAGS $GSTREAMER_CFLAGS"
LIBS="$LIBS $GSTREAMER_LIBS"
AC_CHECK_FUNCS([gst_audio_info_from_caps])
CFLAGS=$mdw_SAVE_CFLAGS
LIBS=$mdw_SAVE_LIBS

# Checks for libraries.
# We save up a list of missing libraries that we can't do without
# and report them all at once.
AC_CHECK_LIB(gc, GC_malloc,            [AC_SUBST(LIBGC,[-lgc])],
            [missing_libraries="$missing_libraries libgc"])
mdw_SAVE_LIBS=$LIBS
LIBS="$LIBS $LIBGC"
AC_CHECK_FUNCS(GC_get_all_interior_pointers)
LIBS=$mdw_SAVE_LIBS
AC_CHECK_LIB(gcrypt, gcry_md_open,
             [AC_SUBST(LIBGCRYPT,[-lgcrypt])],
            [missing_libraries="$missing_libraries libgcrypt"])
PKG_CHECK_MODULES([PCRE2], [libpcre2-8], [have_pcre2=yes], [have_pcre2=no])
case $have_pcre2 in
  yes)
    CFLAGS="$CFLAGS $PCRE2_CFLAGS" LIBPCRE=$PCRE2_LIBS
    AC_DEFINE([HAVE_LIBPCRE2], [1], [Define if building with libpcre2.])
    ;;
  no)
    AC_CHECK_LIB(pcre, pcre_compile,
                 [AC_SUBST(LIBPCRE,[-lpcre])],
                 [missing_libraries="$missing_libraries libpcre"])
    ;;
esac
if test $want_alsa = yes; then
  AC_CHECK_LIB([asound], [snd_pcm_open],
               [AC_SUBST(LIBASOUND,[-lasound])])
fi
if test $want_pulseaudio = yes; then
  PKG_CHECK_MODULES([PULSEAUDIO],[libpulse],
                    [AC_DEFINE([HAVE_PULSEAUDIO],[1],[define to 1 for PulseAudio support])],
                    [missing_libraries="$missing_libraries libpulse"])
  PKG_CHECK_MODULES([PULSEAUDIO_SIMPLE],[libpulse-simple],,
                    [missing_libraries="$missing_libraries libpulse-simple"])
fi
AC_CHECK_LIB([samplerate],[src_new],
             [AC_SUBST([LIBSAMPLERATE],[-lsamplerate])])
if test $want_server = yes; then
  RJK_CHECK_LIB(db, db_create, [#include <db.h>],
	       [AC_SUBST(LIBDB,[-ldb])],
	       [missing_libraries="$missing_libraries libdb"])
  AC_CHECK_LIB(vorbis, vorbis_info_clear,
	       [:],
	       [missing_libraries="$missing_libraries libvorbis"])
  AC_CHECK_LIB(vorbisfile, ov_open,
	       [AC_SUBST(LIBVORBISFILE,["-lvorbisfile -lvorbis"])],
	       [missing_libraries="$missing_libraries libvorbisfile"],
	       [-lvorbis])
  AC_CHECK_LIB(mad, mad_stream_init,
	       [AC_SUBST(LIBMAD,[-lmad])],
	       [missing_libraries="$missing_libraries libmad"])
  AC_CHECK_LIB([FLAC], [FLAC__stream_decoder_new],
	       [AC_SUBST(LIBFLAC,[-lFLAC])],
	       [missing_libraries="$missing_libraries libFLAC"])
fi
AC_CHECK_LIB([pthread], [pthread_create],
             [AC_SUBST(LIBPTHREAD,[-lpthread])],
	     [missing_libraries="$missing_libraries libpthread"])

if test $want_gtk = yes; then
  if test $want_gtkosx = yes; then
    GTK_CFLAGS="-isystem /Library/Frameworks/Gtk.framework/Headers -isystem /Library/Frameworks/Cairo.framework/Headers"
    GLIB_CFLAGS="-isystem /Library/Frameworks/GLib.framework/Headers"
    GTK_LIBS="-framework Gtk -framework GLib"
  else
    AM_PATH_GLIB_2_0([],[],[missing_libraries="$missing_libraries libglib"])
    AM_PATH_GTK_2_0([],[],[missing_libraries="$missing_libraries libgtk"])
  fi
fi

# Some platforms have iconv already
AC_CHECK_FUNC(iconv_open, [:],
              [RJK_CHECK_LIB(iconv, iconv_open, [#include <iconv.h>],
                            [AC_SUBST(LIBICONV,[-liconv])],
                            [missing_functions="$missing_functions iconv_open"])])
AC_CHECK_FUNC([gethostbyname],[:],[
  AC_CHECK_LIB(nsl,gethostbyname,
               [AC_SUBST(LIBNSL,[-lnsl])],
               [missing_functions="$missing_functions gethostbyname"])])
AC_CHECK_FUNC([socket],[:],[
  AC_CHECK_LIB(socket,socket,
               [AC_SUBST(LIBSOCKET,[-lsocket])],
               [missing_functions="$missing_functions socket"])])
AC_CHECK_FUNC([dlopen],[:],[
  AC_CHECK_LIB(dl,dlopen,
	       [AC_SUBST(LIBDL,[-ldl])],
	       [missing_functions="$missing_functions dlopen"])])

if test ! -z "$missing_libraries"; then
  AC_MSG_ERROR([missing libraries:$missing_libraries])
fi

# Checks for header files.
RJK_FIND_GC_H
if test $want_oss = yes; then
  AC_CHECK_HEADERS([sys/soundcard.h])
fi
if test $want_alsa = yes; then
  AC_CHECK_HEADERS([alsa/asoundlib.h])
fi
if test $want_coreaudio = yes; then
  AC_CHECK_HEADERS([CoreAudio/AudioHardware.h])
fi
AC_CHECK_HEADERS([inttypes.h sys/time.h sys/socket.h netinet/in.h \
                  arpa/inet.h sys/un.h netdb.h pwd.h langinfo.h])
# We don't bother checking very standard stuff
# Compilation will fail if any of these headers are missing, so we
# check for them here and fail early.
if test $want_server = yes; then
  AC_CHECK_HEADERS([db.h],[:],[
    missing_headers="$missing_headers $ac_header"
  ])
fi
AC_CHECK_HEADERS([dlfcn.h gcrypt.h \
		 getopt.h iconv.h langinfo.h \
		 sys/ioctl.h \
	         syslog.h unistd.h],[:],[
  missing_headers="$missing_headers $ac_header"
])
case $have_pcre2 in
  no)
    AC_CHECK_HEADERS([pcre.h], [:],
        [missing_headers="$missing_headers $ac_header"])
    ;;
esac
AC_CHECK_HEADERS([samplerate.h])

if test ! -z "$missing_headers"; then
  AC_MSG_ERROR([missing headers:$missing_headers])
fi

# We require that libpcre support UTF-8
case $have_pcre2 in
  yes) RJK_REQUIRE_PCRE2_UTF8([$PCRE2_CFLAGS], [$PCRE2_LIBS]) ;;
  no) RJK_REQUIRE_PCRE_UTF8([-lpcre]) ;;
esac

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_SIZE_T
AC_C_INLINE
AC_C_BIGENDIAN
AC_CHECK_TYPES([struct sockaddr_in6],,,[AC_INCLUDES_DEFAULT
#include <netinet/in.h>])

# Figure out how we'll check for devices being mounted and unmounted
AC_CACHE_CHECK([for list of mounted filesystems],[rjk_cv_mtab],[
  if test -e /etc/mtab; then
    rjk_cv_mtab=/etc/mtab
  else
    rjk_cv_mtab=none
  fi
])
if test $rjk_cv_mtab != none; then
  AC_DEFINE_UNQUOTED([PATH_MTAB],["$rjk_cv_mtab"],[path to file containing mount list])
fi

# enable -Werror when we check for certain characteristics:

old_CFLAGS="${CFLAGS}"
CFLAGS="${CFLAGS} $gcc_werror"
AC_CHECK_TYPES([long long,uint32_t,uint16_t,uint8_t,intmax_t,uintmax_t])
CFLAGS="${old_CFLAGS}"

# gcrypt maintainers keep changing everything.  Design your interface
# first, then implement it once, rather than getting it wrong three or
# four times and shipping between each attempt.
AC_CACHE_CHECK([for hash handle type in <grypt.h>],
               [rjk_cv_gcrypt_hash_handle],[
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([AC_INCLUDES_DEFAULT
#include <gcrypt.h>
],
               [gcry_md_hd_t h;])],
               [rjk_cv_gcrypt_hash_handle=gcry_md_hd_t],[
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([AC_INCLUDES_DEFAULT
#include <gcrypt.h>
],
		 [GcryMDHd h;])],
		 [rjk_cv_gcrypt_hash_handle=GcryMDHd],
		 [rjk_cv_gcrypt_hash_handle=GCRY_MD_HD])])])
AC_DEFINE_UNQUOTED([gcrypt_hash_handle],[$rjk_cv_gcrypt_hash_handle],
                   [libgcrypt hash handle type])

AC_CACHE_CHECK([for gcry_error_t in <grypt.h>],
               [rjk_cv_have_gcry_error_t],[
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([AC_INCLUDES_DEFAULT
#include <gcrypt.h>
],
		 [gcry_error_t e;])],
		 [rjk_cv_have_gcry_error_t=yes],
		 [rjk_cv_have_gcry_error_t=no])])
if test $rjk_cv_have_gcry_error_t = yes; then
  AC_DEFINE([HAVE_GCRY_ERROR_T],1,[define if <gcrypt.h> defines gcry_error_t])
fi

# Checks for functions
if test $ac_cv_type_long_long = yes; then
  AC_CHECK_FUNCS([atoll strtoll],[:],[
    missing_functions="$missing_functions $ac_func"
  ])
  # Darwin sometimes fails to declare strtoll (e.g. if you ask for -std=c99)
  AC_CACHE_CHECK([whether strtoll is declared in <stdlib.h>],
                 [rjk_cv_strtoll_declared],[
    AC_EGREP_HEADER([strtoll], [stdlib.h],
                    [rjk_cv_strtoll_declared=yes],
                    [rjk_cv_strtoll_declared=no])])
  if test $rjk_cv_strtoll_declared = yes; then
    AC_DEFINE([DECLARES_STRTOLL],[1],[define if <stdlib.h> declares strtoll])
  fi
  AC_CACHE_CHECK([whether atoll is declared in <stdlib.h>],
                 [rjk_cv_atoll_declared],[
    AC_EGREP_HEADER([atoll], [stdlib.h],
                    [rjk_cv_atoll_declared=yes],
                    [rjk_cv_atoll_declared=no])])
  if test $rjk_cv_atoll_declared = yes; then
    AC_DEFINE([DECLARES_ATOLL],[1],[define if <stdlib.h> declares atoll])
  fi
fi
AC_CHECK_FUNCS([ioctl nl_langinfo strsignal setenv unsetenv],[:],[
  missing_functions="$missing_functions $ac_func"
])
# fsync will do if fdatasync not available
AC_CHECK_FUNCS([fdatasync],[:],[
  AC_CHECK_FUNCS([fsync],
                 [AC_DEFINE([fdatasync],[fsync],[define fdatasync to fsync if not available])],
                 [missing_functions="$missing_functions fdatasync"])])
if test ! -z "$missing_functions"; then
  AC_MSG_ERROR([missing functions:$missing_functions])
fi
# dlsym will do if dlfunc not available
AC_CHECK_FUNCS([dlfunc],[:],[
  AC_DEFINE([dlfunc],[dlsym],[define dlfunc to dlsym if not available])
])
if test ! -z "$missing_functions"; then
  AC_MSG_ERROR([missing functions:$missing_functions])
fi

# Functions we can take or leave
AC_CHECK_FUNCS([fls getfsstat closesocket])

if test $want_server = yes; then
  # <db.h> had better be version 3 or later
  AC_CACHE_CHECK([db.h version],[rjk_cv_db_version],[
    AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
		       #include <db.h>
		       #ifndef DB_VERSION_MAJOR
		       # error cannot determine db version
		       #endif
		       #if DB_VERSION_MAJOR < 4
		       # error inadequate db version
		       #endif
		       #if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR <= 2
		       # error inadequate db version
		       #endif
		      ],[])],
		      [rjk_cv_db_version=ok],
		      [rjk_cv_db_version=inadequate])
    if test $rjk_cv_db_version = ok; then
      AC_PREPROC_IFELSE([AC_LANG_PROGRAM([
                         #include <db.h>
                         #if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR == 6
                         # error http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=510270
                         #endif
                        ],[])],
                        [rjk_cv_db_version=ok],
                        [rjk_cv_db_version=toxic])
    fi
  ])
  case $rjk_cv_db_version in
  ok )
    ;;
  inadequate )
    AC_MSG_ERROR([need db version at least 4.3 (but not 4.6)])
    ;;
  toxic )
    AC_MSG_ERROR([db version 4.6.x does not work - see debian bug 510270])
    ;;
  esac
fi

AM_CONDITIONAL([SERVER], [test x$want_server = xyes])
if test $want_gtk = yes; then
  AC_DEFINE([WITH_GTK], [1], [define if using GTK+])
fi
AM_CONDITIONAL([GTK], [test x$want_gtk = xyes])
AM_CONDITIONAL([GSTDECODE], [test x$want_gstdecode = xyes])

# Some GCC invocations warn for converting function pointers to void *.
# This is fair enough, as it's technically forbidden, but we use dlsym()
# which can pretty much only exist if object and function pointers are
# interconvertable.  So we disable -Werror if need be.
if test ! -z "$gcc_werror" && test "$ac_cv_func_dlfunc" != yes; then
  CFLAGS="${CFLAGS} $gcc_werror"
  AC_CACHE_CHECK([whether function pointers can be converted to void * without a warning],
		 [rjk_cv_function_pointer_cast],[
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([AC_INCLUDES_DEFAULT
  void somefunction(void);],
		      [(void *)somefunction])],
		      [rjk_cv_function_pointer_cast=yes],
		      [rjk_cv_function_pointer_cast=no])])
  if test $rjk_cv_function_pointer_cast = no; then
    gcc_werror=""
  fi
  CFLAGS="${old_CFLAGS}"
fi

if test "x$GCC" = xyes; then
  # We need LLONG_MAX and annoyingly GCC doesn't always give it to us
  # by default.
  AC_CACHE_CHECK([what C version to ask for],[rjk_cv_cstd],[
    AC_TRY_COMPILE([#include <limits.h>],[
      long long n = LLONG_MAX;
    ],[rjk_cv_cstd=default],[
      old_CC="$CC"
      CC="${CC} -std=gnu99"
      AC_TRY_COMPILE([#include <limits.h>],[
        long long n = LLONG_MAX;
      ],[rjk_cv_cstd=gnu99],[rjk_cv_cstd=unknown])
      CC="$old_CC"
    ])
  ])
  case $rjk_cv_cstd in
  default | unknown )
    ;;
  * )
    CC="${CC} -std=${rjk_cv_cstd}"
    ;;
  esac

  # http://gcc.gnu.org/bugzilla/show_bug.cgi?id=29478
  AC_CACHE_CHECK([checking for GCC bug 29478],[rjk_cv_pr29478],[
    old_CC="$CC"
    if test $GCC = yes; then
      CC="$CC -Wall -Werror"
    fi
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([
      static int x(char *f) {
       return *f;
      }
      int z(const char *g) {
	return x((char *)g);
      }],[])],
      [rjk_cv_pr29478=no],
      [rjk_cv_pr29478=yes]
    )
    CC="$old_CC"
  ])
  if test $rjk_cv_pr29478 = yes; then
    gcc_werror=''
  fi

  AC_CACHE_CHECK([checking for excessively strict -Wreturn-type],
                 [rjk_cv_gcc44_stupidity],[
    old_CC="$CC"
    if test $GCC = yes; then
      CC="$CC -Wreturn-type -Werror"
    fi
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],[
      static void *threadfn(void) {
        for(;;)
          ;
      }],[])],
      [rjk_cv_gcc44_stupidity=no],
      [rjk_cv_gcc44_stupidity=yes])
    CC="$old_CC"
  ])
  if test $rjk_cv_gcc44_stupidity = yes; then
    AC_DEFINE([HAVE_STUPID_GCC44],[1],[Define if your compiler has excessively strict -Wreturn-type])
  fi

  # a reasonable default set of warnings
  CC="${CC} -Wall -W -Wpointer-arith \
	-Wwrite-strings -Wmissing-prototypes \
	-Wmissing-declarations -Wnested-externs"

  # Fix up GTK+ and GLib compiler flags
  GTK_CFLAGS="`echo \"$GTK_CFLAGS\"|sed 's/-I/-isystem /g'`"
  GLIB_CFLAGS="`echo \"$GLIB_CFLAGS\"|sed 's/-I/-isystem /g'`"

  if test "$gcc_werror" != ''; then
    # GCC 2.95 doesn't know to ignore warnings from system headers
    AC_CACHE_CHECK([whether -Werror is usable],
		    rjk_cv_werror, [
      save_CFLAGS="${CFLAGS}"
      CFLAGS="${CFLAGS} ${GTK_CFLAGS} -Werror"
      AC_TRY_COMPILE([#if WITH_GTK
 #include <gtk/gtk.h>
#endif

struct s { int a, b; };
const struct s sv = { .a = 1 };],
		     [],
		     [rjk_cv_werror=yes],
		     [rjk_cv_werror=no])
      CFLAGS="${save_CFLAGS}"
    ])
    if test $rjk_cv_werror = no; then
      gcc_werror=''
    fi
  fi
  CC="${CC} $gcc_werror"

  # for older GCCs that don't know %ju (etc)
  AC_CACHE_CHECK([whether -Wno-format is required],
		 rjk_cv_noformat,
		 AC_TRY_COMPILE([#include <stdio.h>
#include <stdint.h>
],
			        [printf("%ju", (uintmax_t)0);],
				[rjk_cv_noformat=no],
				[rjk_cv_noformat=yes]))
  if test $rjk_cv_noformat = yes; then
    CC="${CC} -Wno-format"
  fi

  AC_CACHE_CHECK([whether -Wshadow is OK],
		 rjk_cv_shadow,
                 oldCC="${CC}"
		 CC="${CC} -Wshadow"
		 [AC_TRY_COMPILE([
#include <unistd.h>
#include <vorbis/vorbisfile.h>
],
				[],
				[rjk_cv_shadow=yes],
				[rjk_cv_shadow=no])
		 CC="${oldCC}"])
  if test $rjk_cv_shadow = yes; then
    CC="${CC} -Wshadow"
  fi
fi

RJK_GCOV

AH_BOTTOM([#ifdef __GNUC__
# define attribute(x) __attribute__(x)
#else
# define attribute(x)
#endif])

AC_CONFIG_FILES([Makefile
		 images/Makefile
		 scripts/Makefile
		 common/Makefile
		 lib/Makefile
		 server/Makefile
		 cgi/Makefile
		 clients/Makefile
		 disobedience/Makefile
		 disobedience/manual/Makefile
		 doc/Makefile
		 templates/Makefile
		 plugins/Makefile
		 debian/Makefile
		 sounds/Makefile
		 python/Makefile
		 examples/Makefile
		 libtests/Makefile
		 tests/Makefile])
AC_OUTPUT

# A hack: communicate our selection of GStreamer version back to the
# Debian packaging machinery so that it can build the right dependencies.
echo '### extra control substitutions for Debian' >debian/substvars.auto.new
case $mdw_gstreamer_version in
  nil) ;;
  *) echo >>debian/substvars.auto.new \
        "disorder:GstVersion=gstreamer$mdw_gstreamer_version" ;;
esac
mv debian/substvars.auto.new debian/substvars.auto

if test $GCC = yes && test "$gcc_werror" = ''; then
  AC_MSG_WARN([building without -Werror])
fi
if test $want_python = no; then
  AC_MSG_WARN([cannot run the test suit without Python])
fi
if test $want_server = yes && test "$ac_cv_lib_samplerate_src_new" != yes; then
  AC_MSG_WARN([libsamplerate will be required in a future version])
fi

# Local Variables:
# indent-tabs-mode:nil
# End:
