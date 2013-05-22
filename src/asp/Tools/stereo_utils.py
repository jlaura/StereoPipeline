#!/usr/bin/env python
# __BEGIN_LICENSE__
#  Copyright (c) 2009-2013, United States Government as represented by the
#  Administrator of the National Aeronautics and Space Administration. All
#  rights reserved.
#
#  The NGT platform is licensed under the Apache License, Version 2.0 (the
#  "License"); you may not use this file except in compliance with the
#  License. You may obtain a copy of the License at
#  http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
# __END_LICENSE__


import sys, optparse, subprocess, re, os
import os.path as P

# Utilities to ensure that the Python parser does not garble negative
# integers such as '-365' into '-3'.
escapeStr='esc_rand_str'
def escape_vals(vals):
    for index, val in enumerate(vals):
        p = re.match("^-\d+$", val)
        if p:
            vals[index] = escapeStr + val
    return vals
def unescape_vals(vals):
    for index, val in enumerate(vals):
        p = re.match("^" + escapeStr + "(-\d+)$", val)
        if p:
            vals[index] = p.group(1)
    return vals

# Custom option parser that will ignore unknown options
class PassThroughOptionParser(optparse.OptionParser):
    def _process_args( self, largs, rargs, values ):

        rargs=escape_vals(rargs)
        largs=escape_vals(largs)

        while rargs:
            try:
                optparse.OptionParser._process_args(self,largs,rargs,values)
            except (optparse.BadOptionError), e:
                largs.append(e.opt_str)

# Have a stereo executable parse and display all the settings
# being passed to it.
def get_settings( args, **kw ):
    libexecpath = P.join(kw.get('path', P.dirname(P.abspath(__file__))), '..',\
                         'libexec', 'stereo_parse')
    call = [libexecpath]
    call.extend(args)

    try:
        p = subprocess.Popen(call, stdout=subprocess.PIPE)
    except OSError, e:
        raise Exception('%s: %s' % (libexecpath, e))
    (stdout, stderr) = p.communicate()

    p.wait()
    if p.returncode != 0:
        raise Exception('Failed getting stereo settings')
    data = {}
    for line in stdout.split('\n'):
        if "," in line:
            keywords = line.split(',')
            data[keywords[0]] = keywords[1:]

    return data

def die(msg, code=-1):
    print >>sys.stderr, msg
    sys.exit(code)

class BBox:
    def __init__(self, x, y, width, height):
        self.x = x
        self.y = y
        self.width = width
        self.height = height

    def name_str(self):
        return "%i_%i_%i_%i" % ( self.x, self.y, self.width, self.height )

    def crop_str(self):
        return ["--trans-crop-win",str(self.x),
                str(self.y),str(self.width),str(self.height)]

    def expand( self, px, settings ):
        image_size = settings["trans_left_image"]
        self.x = self.x - px
        self.y = self.y - px
        self.width = self.width + 2 * px
        self.height = self.height + 2 * px
        if self.x < 0:
            self.x = 0
        if self.y < 0:
            self.y = 0
        if self.x + self.width > image_size[0]:
            self.width =  image_size - self.x
        if self.y + self.height > image_size[1]:
            self.height = image_size - self.y

def intersect_boxes(A, B):
    axmin = A.x; axmax = A.x + A.width; aymin = A.y; aymax = A.y + A.height
    bxmin = B.x; bxmax = B.x + B.width; bymin = B.y; bymax = B.y + B.height
    xmin = max(axmin, bxmin); xmax = min(axmax, bxmax)
    ymin = max(aymin, bymin); ymax = min(aymax, bymax)
    C = BBox(0, 0, 0, 0)
    C.x = xmin; C.width = xmax - xmin
    if (C.width  < 0): C.width = 0
    C.y = ymin; C.height = ymax - ymin
    if (C.height < 0): C.height = 0
    return C

# Find if a program is in the path
def which(program):
    def is_exe(fpath):
        return os.path.isfile(fpath) and os.access(fpath, os.X_OK)

    fpath, fname = os.path.split(program)
    if fpath:
        if is_exe(program):
            return program
    else:
        for path in os.environ["PATH"].split(os.pathsep):
            path = path.strip('"')
            exe_file = os.path.join(path, program)
            if is_exe(exe_file):
                return exe_file

    return None

def run(bin, args, opt, **kw):
    binpath = P.join(kw.get('path', P.dirname(P.abspath(__file__))), '..', 'bin', bin)
    call = [binpath]
    call.extend(args)

    if opt.dryrun:
        print '%s' % ' '.join(call)
        return
    try:
        code = subprocess.call(call)
    except OSError, e:
        raise Exception('%s: %s' % (binpath, e))
    if code != 0:
        raise Exception('Stereo step ' + kw['msg'] + ' failed')

def run_sparse_disp(args, opt):

    settings   = get_settings( args )
    left_img   = settings["in_file1"]
    right_img  = settings["in_file2"]
    out_prefix = settings["out_prefix"]

    sparse_args = left_img + right_img + out_prefix
    run('sparse_disp', sparse_args, opt, msg='1: Low-res correlation with sparse_disp')