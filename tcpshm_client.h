#pragma once
#include <string>
#include <array>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tcpshm_conn.h"

namespace tcpshm {

template<class Derived, class Conf>
class TcpShmClient
{
public:
    using Connection = TcpShmConnection<Conf>;
    using LoginMsg = LoginMsgTpl<Conf>;
    using LoginRspMsg = LoginRspMsgTpl<Conf>;

protected:
    TcpShmClient(const std::string& client_name, const std::string& ptcp_dir)
        : ptcp_dir_(ptcp_dir) {
        strncpy(client_name_, client_name.c_str(), sizeof(client_name_) - 1);
        mkdir(ptcp_dir_.c_str(), 0755);
        client_name_[sizeof(client_name_) - 1] = 0;
        conn_.init(ptcp_dir.c_str(), client_name_);
    }

    ~TcpShmClient() {
        Stop();
    }

    bool Connect(bool use_shm, const char* server_ipv4, uint16_t server_port) {
        if(!conn_.IsClosed()) {
            static_cast<Derived*>(this)->OnSystemError("already connected", 0);
            return false;
        }
        const char* error_msg;
        if(!server_name_) {
            std::string last_server_name_file = std::string(ptcp_dir_) + "/" + client_name_ + ".lastserver";
            server_name_ = (char*)my_mmap<ServerName>(last_server_name_file.c_str(), false, &error_msg);
            if(!server_name_) {
                static_cast<Derived*>(this)->OnSystemError(error_msg, errno);
                return false;
            }
            strncpy(conn_.GetRemoteName(), server_name_, sizeof(ServerName));
        }
        MsgHeader sendbuf[1 + (sizeof(LoginMsg) + 7) / 8];
        sendbuf[0].size = sizeof(MsgHeader) + sizeof(LoginMsg);
        sendbuf[0].msg_type = LoginMsg::msg_type;
        sendbuf[0].ack_seq = 0;
        LoginMsg* login = (LoginMsg*)(sendbuf + 1);
        strncpy(login->client_name, client_name_, sizeof(login->client_name));
        strncpy(login->last_server_name, server_name_, sizeof(login->last_server_name));
        login->use_shm = use_shm;
        login->client_seq_start = login->client_seq_end = 0;
        if(server_name_[0] &&
           (!conn_.OpenFile(use_shm, &error_msg) ||
            !conn_.GetSeq(&sendbuf[0].ack_seq, &login->client_seq_start, &login->client_seq_end, &error_msg))) {
            static_cast<Derived*>(this)->OnSystemError(error_msg, errno);
            return false;
        }
        int fd;
        if((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            static_cast<Derived*>(this)->OnSystemError("socket", errno);
            return false;
        }
        struct timeval timeout;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;

        if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) < 0) {
            static_cast<Derived*>(this)->OnSystemError("setsockopt SO_RCVTIMEO", errno);
            close(fd);
            return false;
        }

        if(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout)) < 0) {
            static_cast<Derived*>(this)->OnSystemError("setsockopt SO_RCVTIMEO", errno);
            close(fd);
            return false;
        }

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        inet_pton(AF_INET, server_ipv4, &(server_addr.sin_addr));
        server_addr.sin_port = htons(server_port);
        bzero(&(server_addr.sin_zero), 8);

        if(connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
            static_cast<Derived*>(this)->OnSystemError("connect", errno);
            close(fd);
            return false;
        }

        static_cast<Derived*>(this)->FillLoginUserData(&login->user_data);

        int ret = send(fd, sendbuf, sizeof(sendbuf), MSG_NOSIGNAL);
        if(ret != sizeof(sendbuf)) {
            static_cast<Derived*>(this)->OnSystemError("send", ret < 0 ? errno : 0);
            close(fd);
            return false;
        }

        MsgHeader recvbuf[1 + (sizeof(LoginRspMsg) + 7) / 8];
        ret = recv(fd, recvbuf, sizeof(recvbuf), 0);
        if(ret != sizeof(recvbuf)) {
            static_cast<Derived*>(this)->OnSystemError("recv", ret < 0 ? errno : 0);
            close(fd);
            return false;
        }
        LoginRspMsg* login_rsp = (LoginRspMsg*)(recvbuf + 1);
        if(recvbuf[0].size != sizeof(MsgHeader) + sizeof(LoginRspMsg) || recvbuf[0].msg_type != LoginRspMsg::msg_type ||
           login_rsp->server_name[0] == 0) {
            static_cast<Derived*>(this)->OnSystemError("Invalid LoginRsp", 0);
            close(fd);
            return false;
        }
        if(login_rsp->status != 0) {
            if(login_rsp->status == 1) { // seq number mismatch
                static_cast<Derived*>(this)->OnSeqNumberMismatch(sendbuf[0].ack_seq,
                                                                 login->client_seq_start,
                                                                 login->client_seq_end,
                                                                 recvbuf[0].ack_seq,
                                                                 login_rsp->server_seq_start,
                                                                 login_rsp->server_seq_end);
            }
            else {
                static_cast<Derived*>(this)->OnLoginReject(login_rsp);
            }
            close(fd);
            return false;
        }
        login_rsp->server_name[sizeof(login_rsp->server_name) - 1] = 0;
        // check if server name has changed
        if(strncmp(server_name_, login_rsp->server_name, sizeof(ServerName)) != 0) {
            conn_.Release();
            strncpy(server_name_, login_rsp->server_name, sizeof(ServerName));
            strncpy(conn_.GetRemoteName(), server_name_, sizeof(ServerName));
            if(!conn_.OpenFile(use_shm, &error_msg)) {
                static_cast<Derived*>(this)->OnSystemError(error_msg, errno);
                close(fd);
                return false;
            }
            conn_.Reset();
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int64_t now = static_cast<Derived*>(this)->OnLoginSuccess(login_rsp);

        conn_.Open(fd, recvbuf[0].ack_seq, now);
        return true;
    }

    void PollTcp(int64_t now) {
        if(conn_.IsClosed()) return;
        MsgHeader* head = conn_.TcpFront(now);
        if(conn_.IsClosed()) {
            int sys_errno;
            const char* reason = conn_.GetCloseReason(&sys_errno);
            static_cast<Derived*>(this)->OnDisconnected(reason, sys_errno);
            return;
        }
        if(head) {
            static_cast<Derived*>(this)->OnServerMsg(head);
        }
    }

    void PollShm() {
        MsgHeader* head = conn_.ShmFront();
        if(head) {
            static_cast<Derived*>(this)->OnServerMsg(head);
        }
    }

    void Stop() {
        if(server_name_) {
            my_munmap<ServerName>(server_name_);
            server_name_ = nullptr;
        }
        conn_.Release();
    }

    Connection* GetConnection() {
        return &conn_;
    }

private:
    char client_name_[Conf::NameSize];
    using ServerName = std::array<char, Conf::NameSize>;
    char* server_name_ = nullptr;
    std::string ptcp_dir_;
    Connection conn_;
};
} // namespace tcpshm
