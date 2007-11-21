#-*-python-*-
#
# This file is part of DisOrder.
# Copyright (C) 2007 Richard Kettlewell
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
# USA
#

"""Utility module used by tests"""

import os,os.path,subprocess,sys,disorder

def copyfile(a,b):
    """copyfile(A, B)
Copy A to B."""
    open(b,"w").write(open(a).read())

def maketrack(s):
    """maketrack(S)

Make track with relative path S exist"""
    trackpath = "%s/tracks/%s" % (testroot, s)
    trackdir = os.path.dirname(trackpath)
    if not os.path.exists(trackdir):
        os.makedirs(trackdir)
    copyfile("%s/sounds/slap.ogg" % topsrcdir, trackpath)

def stdtracks():
    maketrack("Joe Bloggs/First Album/01:First track.ogg")
    maketrack("Joe Bloggs/First Album/02:Second track.ogg")
    maketrack("Joe Bloggs/First Album/03:Third track.ogg")
    maketrack("Joe Bloggs/First Album/04:Fourth track.ogg")
    maketrack("Joe Bloggs/First Album/05:Fifth track.ogg")
    maketrack("Joe Bloggs/First Album/05:Fifth track.ogg")
    maketrack("Joe Bloggs/Second Album/01:First track.ogg")
    maketrack("Joe Bloggs/Second Album/02:Second track.ogg")
    maketrack("Joe Bloggs/Second Album/03:Third track.ogg")
    maketrack("Joe Bloggs/Second Album/04:Fourth track.ogg")
    maketrack("Joe Bloggs/Second Album/05:Fifth track.ogg")
    maketrack("Joe Bloggs/Second Album/05:Fifth track.ogg")
    maketrack("Joe Bloggs/First Album/01:First track.ogg")
    maketrack("Joe Bloggs/First Album/02:Second track.ogg")
    maketrack("Joe Bloggs/First Album/03:Third track.ogg")
    maketrack("Joe Bloggs/First Album/04:Fourth track.ogg")
    maketrack("Joe Bloggs/First Album/05:Fifth track.ogg")
    maketrack("Fred Smith/Boring/01:Dull.ogg")
    maketrack("Fred Smith/Boring/02:Tedious.ogg")
    maketrack("Fred Smith/Boring/03:Drum Solo.ogg")
    maketrack("Fred Smith/Boring/04:Yawn.ogg")
    maketrack("misc/blahblahblah.ogg")
    maketrack("Various/Greatest Hits/01:Jim Whatever - Spong.ogg")
    maketrack("Various/Greatest Hits/02:Joe Bloggs - Yadda.ogg")

def notracks():
    pass

def start(test):
    """start(TEST)

Start the daemon for test called TEST."""
    global daemon
    assert daemon == None
    if test == None:
        errs = sys.stderr
    else:
        errs = open("%s/%s.log" % (testroot, test), "w")
    server = None
    print " starting daemon"
    daemon = subprocess.Popen(["disorderd",
                               "--foreground",
                               "--config", "%s/config" % testroot],
                              stderr=errs)
    disorder._configfile = "%s/config" % testroot
    disorder._userconf = False

def stop():
    """stop()

Stop the daemon if it has not stopped already"""
    global daemon
    rc = daemon.poll()
    if rc == None:
        print " stopping daemon"
        os.kill(daemon.pid, 15)
        rc = daemon.wait()
    print " daemon has stopped"
    daemon = None

def run(test, setup=None, report=True, name=None): 
    global tests
    tests += 1
    if setup == None:
        setup = stdtracks
    setup()
    start(name)
    try:
        try:
            test()
        except AssertionError, e:
            global failures
            failures += 1
            print e
    finally:
        stop()
    if report:
        if failures:
            print " FAILED"
            sys.exit(1)
        else:
            print " OK"

def remove_dir(d):
    """remove_dir(D)

Recursively delete directory D"""
    if os.path.lexists(d):
        if os.path.isdir(d):
            for dd in os.listdir(d):
                remove_dir("%s/%s" % (d, dd))
            os.rmdir(d)
        else:
            os.remove(d)

# -----------------------------------------------------------------------------
# Common setup

tests = 0
failures = 0
daemon = None
testroot = "%s/testroot" % os.getcwd()
topsrcdir = os.path.abspath(os.getenv("topsrcdir"))
remove_dir(testroot)
os.mkdir(testroot)
open("%s/config" % testroot, "w").write(
"""player *.ogg shell 'echo "$TRACK" >> %s/played.log'
home %s
collection fs ASCII %s/tracks
scratch %s/scratch.ogg
gap 0
stopword 01 02 03 04 05 06 07 08 09 10
stopword 1 2 3 4 5 6 7 8 9
stopword 11 12 13 14 15 16 17 18 19 20
stopword 21 22 23 24 25 26 27 28 29 30
stopword the a an and to too in on of we i am as im for is
username fred
password fredpass
allow fred fredpass
plugins ../plugins
player *.mp3 execraw disorder-decode
player *.ogg execraw disorder-decode
player *.wav execraw disorder-decode
player *.flac execraw disorder-decode
tracklength *.mp3 disorder-tracklength
tracklength *.ogg disorder-tracklength
tracklength *.wav disorder-tracklength
tracklength *.flac disorder-tracklength
""" % (testroot, testroot, testroot, testroot))
copyfile("%s/sounds/scratch.ogg" % topsrcdir,
         "%s/scratch.ogg" % testroot)
