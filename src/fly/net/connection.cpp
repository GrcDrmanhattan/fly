/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *                    _______    _                                     *
 *                   (  ____ \  ( \     |\     /|                      * 
 *                   | (    \/  | (     ( \   / )                      *
 *                   | (__      | |      \ (_) /                       *
 *                   |  __)     | |       \   /                        *
 *                   | (        | |        ) (                         *
 *                   | )        | (____/\  | |                         *
 *                   |/         (_______/  \_/                         *
 *                                                                     *
 *                                                                     *
 *     fly is an awesome c++11 network library.                        *
 *                                                                     *
 *   @author: lichuan                                                  *
 *   @qq: 308831759                                                    *
 *   @email: 308831759@qq.com                                          *
 *   @github: https://github.com/lichuan/fly                           *
 *   @date: 2015-06-22 18:22:00                                        *
 *                                                                     *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <unistd.h>
#include <cstring>
#include <netinet/in.h>
#include "fly/net/connection.hpp"
#include "fly/net/poller_task.hpp"
#include "fly/base/logger.hpp"

extern "C"
{
#include "sha1.h"
}

namespace fly {
namespace net {

//Json
fly::base::ID_Allocator Connection<Json>::m_id_allocator;

Connection<Json>::~Connection()
{
    while(auto *message_chunk = m_recv_msg_queue.pop())
    {
        delete message_chunk;
    }

    while(auto *message_chunk = m_send_msg_queue.pop())
    {
        delete message_chunk;
    }
}

Connection<Json>::Connection(int32 fd, const Addr &peer_addr)
{
    m_fd = fd;
    m_peer_addr = peer_addr;
}

uint64 Connection<Json>::id()
{
    return m_id;
}

void Connection<Json>::send(rapidjson::Document &doc)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    send(buffer.GetString(), buffer.GetSize());
}

void Connection<Json>::send(const void *data, uint32 size)
{
    Message_Chunk *message_chunk = new Message_Chunk(size + sizeof(uint32));
    uint32 *uint32_ptr = (uint32*)message_chunk->read_ptr();
    *uint32_ptr = htonl(size);
    memcpy(message_chunk->read_ptr() + sizeof(uint32), data, size);
    message_chunk->write_ptr(size + sizeof(uint32));
    m_send_msg_queue.push(message_chunk);
    m_poller_task->write_connection(shared_from_this());
}

void Connection<Json>::close()
{
    m_poller_task->close_connection(shared_from_this());
}

const Addr& Connection<Json>::peer_addr()
{
    return m_peer_addr;
}

void Connection<Json>::parse()
{
    while(true)
    {
        char *msg_length_buf = (char*)(&m_cur_msg_length);
        uint32 remain_bytes = sizeof(uint32);
        
        if(m_cur_msg_length != 0)
        {
            goto after_parse_length;
        }
        
        if(m_recv_msg_queue.length() < sizeof(uint32))
        {
            break;
        }
        
        while(auto *message_chunk = m_recv_msg_queue.pop())
        {
            uint32 length = message_chunk->length();
            
            if(length < remain_bytes)
            {
                memcpy(msg_length_buf + sizeof(uint32) - remain_bytes, message_chunk->read_ptr(), length);
                remain_bytes -= length;
                delete message_chunk;
            }
            else
            {
                memcpy(msg_length_buf + sizeof(uint32) - remain_bytes, message_chunk->read_ptr(), remain_bytes);

                if(length == remain_bytes)
                {
                    delete message_chunk;
                }
                else
                {
                    message_chunk->read_ptr(remain_bytes);
                    m_recv_msg_queue.push_front(message_chunk);
                }
                
                break;
            }
        }
        
        m_cur_msg_length = ntohl(m_cur_msg_length);
        
    after_parse_length:
        if(m_recv_msg_queue.length() < m_cur_msg_length)
        {
            break;
        }
        
        const uint32 MAX_MSG_LEN = 102400;
        char msg_buf[MAX_MSG_LEN] = {0};
        char *data = msg_buf;
        bool is_new_buf = false;
        remain_bytes = m_cur_msg_length;
        
        if(m_cur_msg_length > MAX_MSG_LEN)
        {
            LOG_ERROR("message length exceed MAX_MSG_LEN(%d)", MAX_MSG_LEN);
            data = new char[m_cur_msg_length];
            is_new_buf = true;
        }
        else if(m_cur_msg_length > MAX_MSG_LEN / 2)
        {
            LOG_ERROR("message length exceed half of MAX_MSG_LEN(%d)", MAX_MSG_LEN);
        }
        
        while(auto *message_chunk = m_recv_msg_queue.pop())
        {
            uint32 length = message_chunk->length();

            if(length < remain_bytes)
            {
                memcpy(data + m_cur_msg_length - remain_bytes, message_chunk->read_ptr(), length);
                remain_bytes -= length;
                delete message_chunk;
            }
            else
            {
                memcpy(data + m_cur_msg_length - remain_bytes, message_chunk->read_ptr(), remain_bytes);

                if(length == remain_bytes)
                {
                    delete message_chunk;
                }
                else
                {
                    message_chunk->read_ptr(remain_bytes);
                    m_recv_msg_queue.push_front(message_chunk);
                }

                std::unique_ptr<Message<Json>> message(new Message<Json>(shared_from_this()));
                message->m_raw_data.assign(data, m_cur_msg_length);
                m_cur_msg_length = 0;

                if(is_new_buf)
                {
                    delete[] data;
                }
                
                rapidjson::Document &doc = message->doc();
                doc.Parse(message->m_raw_data.c_str());
                
                if(!doc.HasParseError())
                {
                    if(!doc.HasMember("msg_type"))
                    {
                        break;
                    }
                    
                    const rapidjson::Value &msg_type = doc["msg_type"];

                    if(!msg_type.IsUint())
                    {
                        break;
                    }

                    message->m_type = msg_type.GetUint();

                    if(!doc.HasMember("msg_cmd"))
                    {
                        break;
                    }

                    const rapidjson::Value &msg_cmd = doc["msg_cmd"];

                    if(!msg_cmd.IsUint())
                    {
                        break;
                    }

                    message->m_cmd = msg_cmd.GetUint();
                    m_dispatch_cb(std::move(message));
                }
                
                break;
            }
        }
    }
}

//Wsock
fly::base::ID_Allocator Connection<Wsock>::m_id_allocator;

namespace
{
    thread_local SHA1_CTX sha1_ctx;
}

Connection<Wsock>::~Connection()
{
    while(auto *message_chunk = m_recv_msg_queue.pop())
    {
        delete message_chunk;
    }

    while(auto *message_chunk = m_send_msg_queue.pop())
    {
        delete message_chunk;
    }
}

Connection<Wsock>::Connection(int32 fd, const Addr &peer_addr)
{
    m_fd = fd;
    m_peer_addr = peer_addr;
}

uint64 Connection<Wsock>::id()
{
    return m_id;
}

void Connection<Wsock>::send(rapidjson::Document &doc)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    send(buffer.GetString(), buffer.GetSize());
}

void Connection<Wsock>::send(const void *data, uint32 size)
{
    
    // Message_Chunk *message_chunk = new Message_Chunk(size + sizeof(uint32));
    // uint32 *uint32_ptr = (uint32*)message_chunk->read_ptr();
    // *uint32_ptr = htonl(size);
    // memcpy(message_chunk->read_ptr() + sizeof(uint32), data, size);
    // message_chunk->write_ptr(size + sizeof(uint32));
    // m_send_msg_queue.push(message_chunk);
    // m_poller_task->write_connection(shared_from_this());
    Message_Chunk *message_chunk = new Message_Chunk(256);
    char *out_buf = message_chunk->read_ptr();
    memcpy(out_buf, "HTTP/1.1 200 OK\r\n", 17);
    out_buf += 17;
    memcpy(out_buf, "Content-Type:text/plain\r\n", 25);
    out_buf += 25;
    
    // memcpy(out_buf, "Connection:close\r\n", 18);
    // out_buf += 18;
    
    memcpy(out_buf, "Content-Length:2\r\n\r\nok", 22);
    
    message_chunk->write_ptr(64);
    m_send_msg_queue.push(message_chunk);
    m_poller_task->write_connection(shared_from_this());
    //LOG_INFO("out_buf send out...................");
}

void Connection<Wsock>::close()
{
    m_poller_task->close_connection(shared_from_this());
}

const Addr& Connection<Wsock>::peer_addr()
{
    return m_peer_addr;
}

void Connection<Wsock>::parse()
{
    if(m_handshake_phase)
    {
        std::string req;

        while(auto *message_chunk = m_recv_msg_queue.pop())
        {
            req += message_chunk->read_ptr();
        }

        uint32 len = req.length();

        //too short for wsock
        if(len < 100)
        {
            return;
        }

        if(req[len-4] != '\r' || req[len - 3] != '\n' || req[len - 2] != '\r' || req[len - 1] != '\n')
        {
            return;
        }

        std::string::size_type key_pos = req.find("Sec-WebSocket-Key: ");

        if(key_pos == std::string::npos)
        {
            close();
            
            return;
        }

        std::string key_str = req.substr(key_pos + 19, (req.find("\r\n", key_pos + 19) - (key_pos + 19)));
        std::string rsp = "HTTP/1.1 101 Switching Protocols\r\n";
        rsp += "Connection: Upgrade\r\n";
        rsp += "Upgrade: websocket\r\n";
        rsp += "Sec-Websocket-Accept: ";
        const static std::string wsock_magic_key("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        std::string server_key = key_str + wsock_magic_key;
        static thread_local bool sha1_inited = false;
        
        if(!sha1_inited)
        {
            sha1_inited = true;
            sha1_init(&sha1_ctx);
        }
    }
    
    return;
    
    while(true)
    {
        auto *message_chunk = m_recv_msg_queue.pop();
        LOG_INFO("recv from client: %s", message_chunk->read_ptr());
        
        //send(nullptr, 333);c

        return;

        char *msg_length_buf = (char*)(&m_cur_msg_length);
        uint32 remain_bytes = sizeof(uint32);
        
        if(m_cur_msg_length != 0)
        {
            goto after_parse_length;
        }
        
        if(m_recv_msg_queue.length() < sizeof(uint32))
        {
            break;
        }
        
        while(auto *message_chunk = m_recv_msg_queue.pop())
        {
            uint32 length = message_chunk->length();
            
            if(length < remain_bytes)
            {
                memcpy(msg_length_buf + sizeof(uint32) - remain_bytes, message_chunk->read_ptr(), length);
                remain_bytes -= length;
                delete message_chunk;
            }
            else
            {
                memcpy(msg_length_buf + sizeof(uint32) - remain_bytes, message_chunk->read_ptr(), remain_bytes);

                if(length == remain_bytes)
                {
                    delete message_chunk;
                }
                else
                {
                    message_chunk->read_ptr(remain_bytes);
                    m_recv_msg_queue.push_front(message_chunk);
                }
                
                break;
            }
        }
        
        m_cur_msg_length = ntohl(m_cur_msg_length);
        
    after_parse_length:
        if(m_recv_msg_queue.length() < m_cur_msg_length)
        {
            break;
        }
        
        const uint32 MAX_MSG_LEN = 102400;
        char msg_buf[MAX_MSG_LEN] = {0};
        char *data = msg_buf;
        bool is_new_buf = false;
        remain_bytes = m_cur_msg_length;
        
        if(m_cur_msg_length > MAX_MSG_LEN)
        {
            LOG_ERROR("message length exceed MAX_MSG_LEN(%d)", MAX_MSG_LEN);
            data = new char[m_cur_msg_length];
            is_new_buf = true;
        }
        else if(m_cur_msg_length > MAX_MSG_LEN / 2)
        {
            LOG_ERROR("message length exceed half of MAX_MSG_LEN(%d)", MAX_MSG_LEN);
        }
        
        while(auto *message_chunk = m_recv_msg_queue.pop())
        {
            uint32 length = message_chunk->length();

            if(length < remain_bytes)
            {
                memcpy(data + m_cur_msg_length - remain_bytes, message_chunk->read_ptr(), length);
                remain_bytes -= length;
                delete message_chunk;
            }
            else
            {
                memcpy(data + m_cur_msg_length - remain_bytes, message_chunk->read_ptr(), remain_bytes);

                if(length == remain_bytes)
                {
                    delete message_chunk;
                }
                else
                {
                    message_chunk->read_ptr(remain_bytes);
                    m_recv_msg_queue.push_front(message_chunk);
                }

                std::unique_ptr<Message<Wsock>> message(new Message<Wsock>(shared_from_this()));
                message->m_raw_data.assign(data, m_cur_msg_length);
                m_cur_msg_length = 0;

                if(is_new_buf)
                {
                    delete[] data;
                }
                
                rapidjson::Document &doc = message->doc();
                doc.Parse(message->m_raw_data.c_str());
                
                if(!doc.HasParseError())
                {
                    if(!doc.HasMember("msg_type"))
                    {
                        break;
                    }
                    
                    const rapidjson::Value &msg_type = doc["msg_type"];

                    if(!msg_type.IsUint())
                    {
                        break;
                    }

                    message->m_type = msg_type.GetUint();

                    if(!doc.HasMember("msg_cmd"))
                    {
                        break;
                    }

                    const rapidjson::Value &msg_cmd = doc["msg_cmd"];

                    if(!msg_cmd.IsUint())
                    {
                        break;
                    }

                    message->m_cmd = msg_cmd.GetUint();
                    m_dispatch_cb(std::move(message));
                }
                
                break;
            }
        }
    }
}

//Proto
fly::base::ID_Allocator Connection<Proto>::m_id_allocator;

Connection<Proto>::~Connection()
{
    while(auto *message_chunk = m_recv_msg_queue.pop())
    {
        delete message_chunk;
    }

    while(auto *message_chunk = m_send_msg_queue.pop())
    {
        delete message_chunk;
    }
}

Connection<Proto>::Connection(int32 fd, const Addr &peer_addr)
{
    m_fd = fd;
    m_peer_addr = peer_addr;
}

uint64 Connection<Proto>::id()
{
    return m_id;
}

void Connection<Proto>::send(rapidjson::Document &doc)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    send(buffer.GetString(), buffer.GetSize());
}

void Connection<Proto>::send(const void *data, uint32 size)
{
    Message_Chunk *message_chunk = new Message_Chunk(size + sizeof(uint32));
    uint32 *uint32_ptr = (uint32*)message_chunk->read_ptr();
    *uint32_ptr = htonl(size);
    memcpy(message_chunk->read_ptr() + sizeof(uint32), data, size);
    message_chunk->write_ptr(size + sizeof(uint32));
    m_send_msg_queue.push(message_chunk);
    m_poller_task->write_connection(shared_from_this());
}

void Connection<Proto>::close()
{
    m_poller_task->close_connection(shared_from_this());
}

const Addr& Connection<Proto>::peer_addr()
{
    return m_peer_addr;
}

void Connection<Proto>::parse()
{
    while(true)
    {
        auto *message_chunk = m_recv_msg_queue.pop();
        LOG_INFO("recv http length: %d", message_chunk->length());
        LOG_INFO("recv http: %s", message_chunk->read_ptr());
        return;
        
        char *msg_length_buf = (char*)(&m_cur_msg_length);
        uint32 remain_bytes = sizeof(uint32);
        
        if(m_cur_msg_length != 0)
        {
            goto after_parse_length;
        }
        
        if(m_recv_msg_queue.length() < sizeof(uint32))
        {
            break;
        }
        
        while(auto *message_chunk = m_recv_msg_queue.pop())
        {
            uint32 length = message_chunk->length();
            
            if(length < remain_bytes)
            {
                memcpy(msg_length_buf + sizeof(uint32) - remain_bytes, message_chunk->read_ptr(), length);
                remain_bytes -= length;
                delete message_chunk;
            }
            else
            {
                memcpy(msg_length_buf + sizeof(uint32) - remain_bytes, message_chunk->read_ptr(), remain_bytes);

                if(length == remain_bytes)
                {
                    delete message_chunk;
                }
                else
                {
                    message_chunk->read_ptr(remain_bytes);
                    m_recv_msg_queue.push_front(message_chunk);
                }
                
                break;
            }
        }
        
        m_cur_msg_length = ntohl(m_cur_msg_length);
        
    after_parse_length:
        if(m_recv_msg_queue.length() < m_cur_msg_length)
        {
            break;
        }
        
        const uint32 MAX_MSG_LEN = 102400;
        char msg_buf[MAX_MSG_LEN] = {0};
        char *data = msg_buf;
        bool is_new_buf = false;
        remain_bytes = m_cur_msg_length;
        
        if(m_cur_msg_length > MAX_MSG_LEN)
        {
            LOG_ERROR("message length exceed MAX_MSG_LEN(%d)", MAX_MSG_LEN);
            data = new char[m_cur_msg_length];
            is_new_buf = true;
        }
        else if(m_cur_msg_length > MAX_MSG_LEN / 2)
        {
            LOG_ERROR("message length exceed half of MAX_MSG_LEN(%d)", MAX_MSG_LEN);
        }
        
        while(auto *message_chunk = m_recv_msg_queue.pop())
        {
            uint32 length = message_chunk->length();

            if(length < remain_bytes)
            {
                memcpy(data + m_cur_msg_length - remain_bytes, message_chunk->read_ptr(), length);
                remain_bytes -= length;
                delete message_chunk;
            }
            else
            {
                memcpy(data + m_cur_msg_length - remain_bytes, message_chunk->read_ptr(), remain_bytes);

                if(length == remain_bytes)
                {
                    delete message_chunk;
                }
                else
                {
                    message_chunk->read_ptr(remain_bytes);
                    m_recv_msg_queue.push_front(message_chunk);
                }

                std::unique_ptr<Message<Proto>> message(new Message<Proto>(shared_from_this()));
                message->m_raw_data.assign(data, m_cur_msg_length);
                m_cur_msg_length = 0;

                if(is_new_buf)
                {
                    delete[] data;
                }
                
                rapidjson::Document &doc = message->doc();
                doc.Parse(message->m_raw_data.c_str());
                
                if(!doc.HasParseError())
                {
                    if(!doc.HasMember("msg_type"))
                    {
                        break;
                    }
                    
                    const rapidjson::Value &msg_type = doc["msg_type"];

                    if(!msg_type.IsUint())
                    {
                        break;
                    }

                    message->m_type = msg_type.GetUint();

                    if(!doc.HasMember("msg_cmd"))
                    {
                        break;
                    }

                    const rapidjson::Value &msg_cmd = doc["msg_cmd"];

                    if(!msg_cmd.IsUint())
                    {
                        break;
                    }

                    message->m_cmd = msg_cmd.GetUint();
                    m_dispatch_cb(std::move(message));
                }
                
                break;
            }
        }
    }
}

}
}
