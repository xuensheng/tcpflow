# -*- coding: utf-8 -*-
import os
import time
import sys
import oss2
import commands
import re
from subprocess import *

access_key_id = 'LTAIdrzDuhBJeJfA'
access_key_secret = '2qKzGaU3wRofE8dERyjOEdw0zJ4icp'
bucket_name = 'xes-test-live-channel'
endpoint = 'oss-test.aliyun-inc.com'

bucket = oss2.Bucket(oss2.Auth(access_key_id, access_key_secret), endpoint, bucket_name)
bucket.create_bucket(oss2.BUCKET_ACL_PUBLIC_READ)

def run_cmd(cmd):
    proc = Popen(cmd, shell=True, executable="/bin/bash", stdout=PIPE, stderr=PIPE)
    stdout, stderr = proc.communicate()
    status = proc.wait()
    proc.stdout.close()
    proc.stderr.close() 
    if status == 0:
        return True, stdout, stderr
    return False, stdout, stderr

class TsFile:
    def __init__(self, duration, md5sum, name = ''):
        self._duration = duration
        self._md5sum = md5sum
        self._name = name

    def __str__(self):
        return "(duration=%s, md5sum=%s)" % (self._duration, self._md5sum)

def get_md5(channel, fname):
    res = bucket.get_object(channel + '/' + fname)
    return res.etag

def get_object(fname):
    res = bucket.get_object(fname)
    return res.read()

def parse_m3u8(channel, data):
    pat = re.compile(r'#EXTINF:([-0-9.]+),.*?([a-z0-9.-]+.ts)', re.S)
    ts_list = []
    for i, (duration, name) in enumerate(pat.findall(data)):
        ts_list.append(TsFile(duration, get_md5(channel, name), name))
    return ts_list

if __name__ == '__main__':
    cmd = '../src/tcpflow -o /tmp/tcpflowoutput/ -r test-rtmp.cap'
    status, stdout, stderr = run_cmd(cmd)
    assert(status == True)
    
    channel_name = ''
    for line in stderr.splitlines():
        if line.find('post succeed: ') != -1:
            print line
            columns = line.split(' ')
            vodurl = columns[len(columns) - 1]
            channel_name = vodurl.split('/')[-2]
    assert(channel_name != '')
    print channel_name

    ts_list = parse_m3u8(channel_name,
                         get_object(channel_name + '/' + 'vod.m3u8'))

    assert(len(ts_list) == 2)

    assert(ts_list[0]._duration == '5.172')
    assert(ts_list[0]._md5sum == 'D2DD247AD3C677432D4689DE919AA961')

    assert(ts_list[1]._duration == '4.804')
    assert(ts_list[1]._md5sum == '054A318568E83998E3C2BDE6AFE35529')

    print 'test OK'

    
