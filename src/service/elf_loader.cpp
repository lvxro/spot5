// spot5 - Starting a payload through the ELF loader, and talking to it.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "service/elf_loader.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>

#include "app/config.hpp"
#include "platform/ps5/system.hpp"

namespace service
{

namespace
{
constexpr int kElfLoaderPort = 9021;
} // namespace

int connect_loopback(int port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
        return fd;
    if (fd >= 0)
        close(fd);
    return -1;
}

bool send_to_elf_loader(const std::string &path, const std::string &what, std::string &error)
{
    const std::string elf = app::read_file(path);
    if (elf.empty())
    {
        error = "Missing " + path;
        return false;
    }
    const int fd = connect_loopback(kElfLoaderPort);
    if (fd < 0)
    {
        error = what + " needs the ELF loader running (port 9021)";
        return false;
    }
    size_t sent = 0;
    while (sent < elf.size())
    {
        const ssize_t now = send(fd, elf.data() + sent, elf.size() - sent, 0);
        if (now <= 0)
            break;
        sent += static_cast<size_t>(now);
    }
    close(fd); // end of file: elfldr starts it
    if (sent != elf.size())
    {
        error = what + ": the ELF loader did not take " + path;
        return false;
    }
    return true;
}

int open_ready_link(int port, int seconds)
{
    for (int attempt = 0; attempt < seconds * 5; ++attempt)
    {
        const int fd = connect_loopback(port);
        if (fd >= 0)
        {
            timeval timeout{1, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            char ready[6];
            if (recv(fd, ready, sizeof(ready), MSG_WAITALL) == 6 && std::memcmp(ready, "READY\n", 6) == 0)
                return fd;
            close(fd); // a payload on its way out
        }
        hui::sys::sleep_us(200000);
    }
    return -1;
}

} // namespace service
