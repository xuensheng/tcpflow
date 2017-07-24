# -*- coding: utf-8 -*-

import os
import time
import sys

import oss2
import urlparse
from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
import commands

access_key_id = '*'
access_key_secret = '*'
bucket_name = 'xes-test-live-channel'
endpoint = 'oss-test.aliyun-inc.com'
oss_ip = '10.101.225.220'

# 创建Bucket对象，所有直播相关的接口都可以通过Bucket对象来进行
bucket = oss2.Bucket(oss2.Auth(access_key_id, access_key_secret), endpoint, bucket_name)

bucket.create_bucket(oss2.BUCKET_ACL_PUBLIC_READ)

class MyHTTPRequestHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed_result = urlparse.urlparse(self.path)
        url_path = parsed_result.path
        if url_path != 'publish':
            channel_name = 'test_rtmp_live_%d' % (time.time())
            playlist_name = 'test.m3u8'
            create_result = bucket.create_live_channel(
                            channel_name,
                            oss2.models.LiveChannelInfo(
                            status = 'enabled',
                            description = '测试使用的直播频道',
                            target = oss2.models.LiveChannelInfoTarget(
                                playlist_name = playlist_name,
                                frag_count = 3,
                                frag_duration = 5),
                                snapshot = oss2.models.LiveChannelSnapshotConfig(
                                    notify_topic = 'mytopic',
                                    role = 'roleforossmaru',
                                    dest_bucket = bucket_name)))

            expires = 360000
            signed_url = bucket.sign_rtmp_url(channel_name, playlist_name, expires)
            print '%s' % signed_url

            self.send_response(200)
            self.send_header('Content-Length', str(len(signed_url)))
            self.end_headers()
            self.wfile.write(signed_url)
            return

    def do_POST(self):
        parsed_result = urlparse.urlparse(self.path)
        url_path = parsed_result.path
        if url_path != 'postvodlist':
            start_time = int(self.headers['x-oss-start-time']) - 100
            channel_name = self.headers['x-oss-channel-id']
            end_time = int(time.time()) + 100
            playurl = ''

            try:
                bucket.post_vod_playlist(channel_name,
                         'vod.m3u8',
                         start_time = start_time,
                         end_time = end_time)
                if len(oss_ip):
                    playurl = 'http://' + oss_ip + '/' + bucket_name + '/'  + channel_name + '/vod.m3u8'
                else:
                    playurl = bucket_name + '.' + endpoint + '/'  + channel_name + '/vod.m3u8'
            except:
                pass

            bucket.delete_live_channel(channel_name)

            if len(playurl) > 0:
                self.send_response(200)
                self.send_header('Content-Length', str(len(playurl)))
                self.end_headers()
                self.wfile.write(playurl)
            else:
                self.send_response(400)
                self.end_headers()

class MyHTTPServer(HTTPServer):
    def __init__(self, host, port):
        HTTPServer.__init__(self, (host, port), MyHTTPRequestHandler)

server = MyHTTPServer('127.0.0.1', 7123)
server.serve_forever()

