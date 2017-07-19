#include "tcpflow.h"
#include "tcpip.h"
#include "rtmpparser.h"

#include <iostream>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<netdb.h>
#include<sys/types.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<unistd.h>
#include<arpa/inet.h>
#include<errno.h>


int rtmpparser::init()
{
    if((sockfd = socket(AF_INET,SOCK_STREAM,0)) == -1) {
        std::cerr << "Socket Error: " << strerror(errno) << std::endl;
        return -1;
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr,sizeof(server_addr));
    server_addr.sin_family=AF_INET;
    server_addr.sin_port=htons(1935);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if(connect(sockfd, (struct sockaddr *)(&server_addr), sizeof(struct sockaddr)) == -1) {
        std::cerr << "Connect Error: " << strerror(errno) << std::endl;
        return -1;
    }

    return 0;
}

int rtmpparser::send_data(const char* buf, size_t size)
{
    int ret;
    int left = size;

    char recv_buf[16*1024];
    do {
        ret = recv(sockfd, recv_buf, 16 * 1024, MSG_DONTWAIT);
        if (ret == 0) {
            usleep(100*1024);
            ret = recv(sockfd, recv_buf, 16 * 1024, MSG_DONTWAIT);
        }
    } while(ret > 0);

    while(left > 0) {
        ret = send(sockfd, buf + size - left, left, 0);
        if ( ret < 0) {
            close(sockfd);
            sockfd = -1;
            return -1;
        } else if (ret != left) {
            usleep(10*1000);
            continue;
        }

        left -= ret;
    }
    return 0;
}

int rtmpparser::recv_data(char** buf, int* size, int wait)
{
    sleep(wait);

    char * recv_buf = new char[16*1024];
    int len = 0;
    int ret;
    do {
        ret = recv(sockfd, recv_buf + len, 16 * 1024 - len, MSG_DONTWAIT);
        if (ret > 0)
            len += ret;
        if (len >= 16 * 1024)
            break;
    } while(ret > 0);

    if (buf != NULL && size != NULL) {
        *size = len;
        *buf = recv_buf;
    } else {
        delete recv_buf;
    }

    return 0;
}

struct rtmp_header {
    unsigned char type;
    unsigned char ts[3];
    unsigned char amf_size[3];
    unsigned char amf_type;
    unsigned char stream_id[4];
};

#define CONNECT_URL "rtmp://xes-test-live-channel.oss-test.aliyun-inc.com:1935/live"
#define CONNECT_URL_SIZE (strlen(CONNECT_URL))

typedef enum {                       
    AMF_DATA_TYPE_NUMBER      = 0x00,
    AMF_DATA_TYPE_BOOL        = 0x01,
    AMF_DATA_TYPE_STRING      = 0x02,
    AMF_DATA_TYPE_OBJECT      = 0x03,
    AMF_DATA_TYPE_NULL        = 0x05,
    AMF_DATA_TYPE_UNDEFINED   = 0x06,
    AMF_DATA_TYPE_REFERENCE   = 0x07,
    AMF_DATA_TYPE_MIXEDARRAY  = 0x08,
    AMF_DATA_TYPE_OBJECT_END  = 0x09,
    AMF_DATA_TYPE_ARRAY       = 0x0a,
    AMF_DATA_TYPE_DATE        = 0x0b,
    AMF_DATA_TYPE_LONG_STRING = 0x0c,
    AMF_DATA_TYPE_UNSUPPORTED = 0x0d,
} AMFDataType;                       

static uint16_t AV_RB16(const uint8_t *p)
{
    return (p[0] << 8)  + p[1];
}

static uint32_t AV_RB24(const uint8_t *p)
{
    return (p[0] << 16)  + (p[1] << 8) + p[2];
}

static uint32_t AV_RB32(const uint8_t *p)
{
    return (p[0] << 24)  + (p[1] << 16) + (p[2] << 8) + p[3];
}


#define DEF(type, name, bytes, read)                                           \
static type bytestream_get_ ## name(const uint8_t **b)        \
{                                                                              \
    (*b) += bytes;                                                             \
    return read(*b - bytes);                                                   \
}        

DEF(unsigned int, be16, 2, AV_RB16)
DEF(unsigned int, be32, 4, AV_RB32)

#define min(a,b) ((a) < (b) ? (a) : (b))

int ff_amf_tag_size(const uint8_t *data, const uint8_t *data_end)
{
    const uint8_t *base = data;
    AMFDataType type;
    unsigned nb   = -1;
    int parse_key = 1;

    if (data >= data_end)
        return -1;
    int t = *data++;
    switch ((type = (AMFDataType)t)) {
    case AMF_DATA_TYPE_NUMBER:      return 9;
    case AMF_DATA_TYPE_BOOL:        return 2;
    case AMF_DATA_TYPE_STRING:      return 3 + AV_RB16(data);
    case AMF_DATA_TYPE_LONG_STRING: return 5 + AV_RB32(data);
    case AMF_DATA_TYPE_NULL:        return 1;
    case AMF_DATA_TYPE_DATE:        return 11;
    case AMF_DATA_TYPE_ARRAY:
        parse_key = 0;
    case AMF_DATA_TYPE_MIXEDARRAY:
        nb = bytestream_get_be32(&data);
    case AMF_DATA_TYPE_OBJECT:
        while (nb-- > 0 || type != AMF_DATA_TYPE_ARRAY) {
            int t;
            if (parse_key) {
                int size = bytestream_get_be16(&data);
                if (!size) {
                    data++;
                    break;
                }
                if (size < 0 || size >= data_end - data)
                    return -1;
                data += size;
            }
            t = ff_amf_tag_size(data, data_end);
            if (t < 0 || t >= data_end - data)
                return -1;
            data += t;
        }
        return data - base;
    case AMF_DATA_TYPE_OBJECT_END:  return 1;
    default:                        return -1;
    }
}

int ff_amf_get_field_value(const uint8_t *data, const uint8_t *data_end,
                           const uint8_t *name, uint8_t *dst, int dst_size)
{
    int namelen = strlen((const char*)name);
    int len;

    while (*data != AMF_DATA_TYPE_OBJECT && data < data_end) {
        len = ff_amf_tag_size(data, data_end);
        if (len < 0)
            len = data_end - data;
        data += len;
    }
    if (data_end - data < 3)
        return -1;
    data++;
    for (;;) {
        int size = bytestream_get_be16(&data);
        if (!size)
            break;
        if (size < 0 || size >= data_end - data)
            return -1;
        data += size;
        if (size == namelen && !memcmp(data-size, name, namelen)) {
            switch (*data++) {
            case AMF_DATA_TYPE_NUMBER:
                break;
            case AMF_DATA_TYPE_BOOL:
                break;
            case AMF_DATA_TYPE_STRING:
                len = bytestream_get_be16(&data);
                strncpy((char*)dst, (char*)data, min(len+1, dst_size));
                dst[min(len+1, dst_size)] = '\0';
                break;
            default:
                return -1;
            }
            return 0;
        }
        len = ff_amf_tag_size(data, data_end);
        if (len < 0 || len >= data_end - data)
            return -1;
        data += len;
    }
    return -1;
}

const char* gen_publish_url()
{
    return "";
}

char* replace_buf(const char* buf, int buf_size, const char* org, int org_size, const char* to, int to_size)
{
    char* new_buf = new char[buf_size + to_size];

    std::cerr << "replace: " << org << " to " << to << std::endl;

    int len = org - buf;
    memcpy(new_buf, buf, len);
    memcpy(new_buf + len, to, to_size);
    memcpy(new_buf + len + to_size, org + org_size, buf_size - len - org_size);
    
    return new_buf;
}

const char* memstr(const char* buf, int buf_size, const char* str)
{
    int len = strlen(str);
    for (int i = 0; i < buf_size - len; i++) {
        if (memcmp(buf + i, str, len) == 0)
            return buf + i;
    }

    return NULL;
}

int get_header_size(unsigned char type)
{
    int fmt = (type & 0xc0) >> 6;
    int header_size;

    std::cerr << "head " << fmt << " " << (int)type << std::endl;

    if (fmt == 0)
        header_size = 12;
    else if (fmt == 1)
        header_size = 8;
    else if (fmt == 2)
        header_size = 4;
    else if (fmt = 3)
        header_size = 1;

    return header_size;
}

int rtmpparser::process_packet(const char* buf, size_t size)
{
    if (sockfd < 0 || size < 1)
        return -1;

    const char* orgbuf = buf;

    std::cerr << flowinfo << "recv pkt, size: " << size << " total processed: " << processed_size << "\n";
    
    if (processed_size < 3073) {
        send_data(buf, size);
        processed_size += size;
        return 0;
    }

    while(size > 0) {
        if (expect_pkt_buf_size > pkt_buf_max_size) {
            pkt_buf = (char*)realloc(pkt_buf, expect_pkt_buf_size);
            pkt_buf_max_size = expect_pkt_buf_size;
        }

        int copy_size = min(expect_pkt_buf_size - pkt_buf_size, size);
        memcpy(pkt_buf + pkt_buf_size, buf, copy_size);
        int parsed = buf - orgbuf;
        buf += copy_size;
        size -= copy_size;
        pkt_buf_size += copy_size;
        std::cerr << "copy from " << parsed << " size " << copy_size << " left " << size << std::endl;

        if(pkt_buf_size < expect_pkt_buf_size) {
            continue;
        }

        assert(pkt_buf_size == expect_pkt_buf_size);

        rtmp_header* head = (rtmp_header *)pkt_buf;
        int header_size = get_header_size(head->type);
        int body_size = 0;

        std::cerr << "pkt_buf_size: " << pkt_buf_size << " expect_pkt_buf_size " << expect_pkt_buf_size  << " header_size " << header_size << std::endl;

        if (pkt_buf_size < header_size) {
            expect_pkt_buf_size = header_size;
            std::cerr << "wait recv header, head_size: " << header_size << std::endl;
            continue;
        }
        
        if (header_size < 8) {
            send_data(pkt_buf, pkt_buf_size);
            processed_size += pkt_buf_size;
            pkt_buf_size = 0;
            continue;
        }

        body_size = AV_RB24(head->amf_size);
        if (header_size >= 8 && pkt_buf_size == header_size) {
            if (body_size <= 128) {
                expect_pkt_buf_size = header_size + body_size;
                payload_size = body_size;
            } else {
                expect_pkt_buf_size = header_size + 128 + 1;
                payload_size = 128;
            }

            std::cerr << "wait recv body, head_size: " << header_size << " body_size: " << body_size << std::endl;
            std::cerr << "body size: " << body_size << " payload size: " << payload_size << std::endl;
            continue;
        }
        
        if (payload_size < body_size) {
            rtmp_header* head2 = (rtmp_header *)&pkt_buf[pkt_buf_size - 1];
            int header_size2 = get_header_size(head2->type);
            assert(header_size2 == 1);
            int left = body_size - payload_size;
            if (left <= 128) {
                expect_pkt_buf_size += (header_size2 - 1) + left;
            } else {
                expect_pkt_buf_size += (header_size2 - 1) + 128 + 1;
            }
            payload_size += min(left, 128);
            std::cerr << "body size: " << body_size << " payload size: " << payload_size << std::endl;
            continue;
        } else {
            parse_packet(pkt_buf, pkt_buf_size);
            processed_size += pkt_buf_size;
            pkt_buf_size = 0;
            expect_pkt_buf_size = 1;
        }
    }

}

int rtmpparser::parse_packet(const char* buf, size_t size)
{

    rtmp_header* head = (rtmp_header *)buf;
    int fmt = (head->type & 0xc0) >> 6;
    int header_size = get_header_size(head->type);
    
    int amf_type = head->amf_type;
    int amf_size = (head->amf_size[0] << 16) + (head->amf_size[1] << 8) + head->amf_size[2];

    std::cerr << "header_size: " << header_size << " type: " << amf_type << " body size: " << amf_size << std::endl;
    send_data(buf, size);

    return 0;
    
    if (amf_type == 0x14) { //invoke
        const char* pos = memstr(buf, size, "tcUrl");
        if (pos != NULL) {
            //rewrite tcurl
            std::cerr << "pos: " << pos << std::endl;
            pos = pos + strlen("tcUrl");
            assert(pos[0] == 0x02);//type string
            pos++;
            int url_size = (pos[0] << 8) + pos[1];
            
            char* new_buf = replace_buf(buf, size, pos + 2, url_size, CONNECT_URL, CONNECT_URL_SIZE);
            int new_size = size - url_size + CONNECT_URL_SIZE;

            head = (rtmp_header *)new_buf;

            head->amf_size[0] = size >> 16;
            head->amf_size[1] = (size >> 8) & 0xff;
            head->amf_size[2] = size & 0xff;
            
            char* new_pos = new_buf + (pos - buf);
            new_pos[0] = CONNECT_URL_SIZE >> 8;
            new_pos[1] = CONNECT_URL_SIZE & 0xff;

            send_data(new_buf, new_size);
            delete new_buf;
            return 0;
        }
        
        pos = memstr(buf, size, "publish");
        if (pos != NULL) {
            //rewrite publish channel
            std::cerr << "pos-publish: " << pos << std::endl;
            pos = pos + strlen("publish") + 10;
            assert(pos[0] == 0x02);//type string
            pos++;
            int url_size = (pos[0] << 8) + pos[1];

            const char* publish_url = gen_publish_url();
            int publish_url_size = strlen(publish_url);

            char* new_buf = replace_buf(buf, size, pos + 2, url_size, publish_url, publish_url_size);
            int new_size = size - url_size + publish_url_size;

            head = (rtmp_header *)new_buf;

            head->amf_size[0] = size >> 16;
            head->amf_size[1] = (size >> 8) & 0xff;
            head->amf_size[2] = size & 0xff;

            char* new_pos = new_buf + (pos - buf);
            new_pos[0] = publish_url_size >> 8;
            new_pos[1] = publish_url_size & 0xff;

            buf = new_buf;
            size = new_size;
            
            send_data(new_buf, new_size);
            delete new_buf;

            char* recv_buf;
            int recv_size = 0;
            recv_data(&recv_buf, &recv_size, 2);
            if (recv_size > 0) {
                char code_buf[1024];
                ff_amf_get_field_value((const uint8_t*)recv_buf,
                                       (const uint8_t*)(recv_buf + recv_size),
                                       (const uint8_t*)"code",
                                       (uint8_t*)code_buf,
                                       sizeof(code_buf));  
                std::cerr << "code: " << code_buf << std::endl;
            }
            return 0;
        }
    }

    send_data(buf, size);

    return 0;
}
