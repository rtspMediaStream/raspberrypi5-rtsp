#ifndef RTSP_UDPHANDLER_H
#define RTSP_UDPHANDLER_H

#include "Protos.h"

#include <string>
#include <arpa/inet.h>
#include <memory>

class ClientSession;

class UDPHandler {
public:
    UDPHandler(std::shared_ptr<ClientSession> client);
    ~UDPHandler();

    bool CreateUDPSocket();

    void SendSenderReport(Protos::SenderReport* senderReport, size_t srSize);

    int& GetRTPSocket();
    int& GetRTCPSocket();

    sockaddr_in& GetRTPAddr();
    sockaddr_in& GetRTCPAddr();


private:
    std::shared_ptr<ClientSession> client;

    int rtpSocket;
    int rtcpSocket;

    sockaddr_in rtpAddr;
    sockaddr_in rtcpAddr;
};

#endif //RTSP_UDPHANDLER_H