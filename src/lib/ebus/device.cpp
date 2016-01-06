/*
 * ebusd - daemon for communication with eBUS heating systems.
 * Copyright (C) 2015-2016 John Baier <ebusd@ebusd.eu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "device.h"
#include "data.h"
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <errno.h>

#ifdef HAVE_PPOLL
#include <poll.h>
#endif

using namespace std;

Device::~Device()
{
	close();
	m_dumpRawStream.close();
}

Device* Device::create(const char* name, const bool checkDevice, const bool readonly,
		void (*logRawFunc)(const unsigned char byte, bool received))
{
	if (strchr(name, '/') == NULL && strchr(name, ':') != NULL) {
		char* dup = strdup(name);
		char* pos = strchr(dup, ':');
		result_t result = RESULT_OK;
		unsigned int port = parseInt(pos+1, 10, 1, 65535, result);
		if (result!=RESULT_OK) {
			free(dup);
			return NULL; // invalid port
		}
		struct sockaddr_in address;
		memset((char*)&address, 0, sizeof(address));

		*pos = 0;
		if (inet_aton(dup, &address.sin_addr) == 0) {
			struct hostent* h = gethostbyname(dup);
			if (h == NULL) {
				free(dup);
				return NULL; // invalid host
			}
			memcpy(&address.sin_addr, h->h_addr_list[0], h->h_length);
		}
		free(dup);
		address.sin_family = AF_INET;
		address.sin_port = (in_port_t)htons((uint16_t)port);
		return new NetworkDevice(name, address, readonly, logRawFunc);
	}
	return new SerialDevice(name, checkDevice, readonly, logRawFunc);
}

void Device::close()
{
	if (m_fd != -1) {
		::close(m_fd);
		m_fd = -1;
	}
}

bool Device::isValid()
{
	if (m_fd == -1)
		return false;

	if (m_checkDevice)
		checkDevice();

	return m_fd != -1;
}

result_t Device::send(const unsigned char value)
{
	if (!isValid())
		return RESULT_ERR_DEVICE;

	if (m_readonly || write(m_fd, &value, 1) != 1)
		return RESULT_ERR_SEND;

	if (m_logRaw && m_logRawFunc != NULL)
		(*m_logRawFunc)(value, false);

	return RESULT_OK;
}

result_t Device::recv(const long timeout, unsigned char& value)
{
	if (!isValid())
		return RESULT_ERR_DEVICE;

	if (timeout > 0) {
		int ret;
		struct timespec tdiff;

		// set select timeout
		tdiff.tv_sec = timeout/1000000;
		tdiff.tv_nsec = (timeout%1000000)*1000;

#ifdef HAVE_PPOLL
		int nfds = 1;
		struct pollfd fds[nfds];

		memset(fds, 0, sizeof(fds));

		fds[0].fd = m_fd;
		fds[0].events = POLLIN;

		ret = ppoll(fds, nfds, &tdiff, NULL);
#else
#ifdef HAVE_PSELECT
		fd_set readfds;

		FD_ZERO(&readfds);
		FD_SET(m_fd, &readfds);

		ret = pselect(m_fd + 1, &readfds, NULL, NULL, &tdiff, NULL);
#else
		ret = 1; // ignore timeout if neither ppoll nor pselect are available
#endif
#endif
		if (ret == -1) return RESULT_ERR_DEVICE;
		if (ret == 0) return RESULT_ERR_TIMEOUT;
	}

	// directly read byte from device
	ssize_t nbytes = read(m_fd, &value, 1);
	if (nbytes == 0)
		return RESULT_ERR_EOF;
	if (nbytes < 0)
		return RESULT_ERR_DEVICE;

	if (m_logRaw && m_logRawFunc != NULL)
		(*m_logRawFunc)(value, true);

	if (m_dumpRaw && m_dumpRawStream.is_open()) {
		m_dumpRawStream.write((char*)&value, 1);
		m_dumpRawFileSize++;
		if ((m_dumpRawFileSize%1024) == 0)
			m_dumpRawStream.flush();

		if (m_dumpRawFileSize >= m_dumpRawMaxSize * 1024) {
			string oldfile = string(m_dumpRawFile) + ".old";
			if (rename(m_dumpRawFile, oldfile.c_str()) == 0) {
				m_dumpRawStream.close();
				m_dumpRawStream.open(m_dumpRawFile, ios::out | ios::binary | ios::app);
				m_dumpRawFileSize = 0;
			}
		}
	}

	return RESULT_OK;
}

void Device::setDumpRaw(bool dumpRaw)
{
	if (dumpRaw == m_dumpRaw)
		return;

	m_dumpRaw = dumpRaw;

	if (!dumpRaw || m_dumpRawFile == NULL)
		m_dumpRawStream.close();
	else {
		m_dumpRawStream.open(m_dumpRawFile, ios::out | ios::binary | ios::app);
		m_dumpRawFileSize = 0;
	}
}

void Device::setDumpRawFile(const char* dumpFile) {
	if ((dumpFile == NULL) ? (m_dumpRawFile == NULL) : (m_dumpRawFile != NULL && (m_dumpRawFile == dumpFile || strcmp(dumpFile, m_dumpRawFile) == 0)))
		return;

	m_dumpRawStream.close();
	m_dumpRawFile = dumpFile;

	if (m_dumpRaw && m_dumpRawFile != NULL) {
		m_dumpRawStream.open(m_dumpRawFile, ios::out | ios::binary | ios::app);
		m_dumpRawFileSize = 0;
	}
}


result_t SerialDevice::open()
{
	if (m_fd != -1)
		close();

	struct termios newSettings;

	// open file descriptor
	m_fd = ::open(m_name, O_RDWR | O_NOCTTY);

	if (m_fd < 0)
		return RESULT_ERR_NOTFOUND;
	if (isatty(m_fd) == 0) {
		close();
		return RESULT_ERR_NOTFOUND;
	}

	if (flock(m_fd, LOCK_EX|LOCK_NB)) {
		close();
		return RESULT_ERR_DEVICE;
	}

	// save current settings
	tcgetattr(m_fd, &m_oldSettings);

	// create new settings
	memset(&newSettings, '\0', sizeof(newSettings));

	newSettings.c_cflag |= (B2400 | CS8 | CLOCAL | CREAD);
	newSettings.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // non-canonical mode
	newSettings.c_iflag |= IGNPAR; // ignore parity errors
	newSettings.c_oflag &= ~OPOST;

	// non-canonical mode: read() blocks until at least one byte is available
	newSettings.c_cc[VMIN]  = 1;
	newSettings.c_cc[VTIME] = 0;

	// empty device buffer
	tcflush(m_fd, TCIFLUSH);

	// activate new settings of serial device
	tcsetattr(m_fd, TCSAFLUSH, &newSettings);

	// set serial device into blocking mode
	fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL) & ~O_NONBLOCK);

	return RESULT_OK;
}

void SerialDevice::close()
{
	if (m_fd != -1) {
		// empty device buffer
		tcflush(m_fd, TCIOFLUSH);

		// restore previous settings of the device
		tcsetattr(m_fd, TCSANOW, &m_oldSettings);
	}
	Device::close();
}

void SerialDevice::checkDevice()
{
	int port;

	if (ioctl(m_fd, TIOCMGET, &port) == -1) {
		close();
	}
}


result_t NetworkDevice::open()
{
	if (m_fd != -1)
		close();

	int ret;

	m_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_fd < 0)
		return RESULT_ERR_GENERIC_IO;

	ret = connect(m_fd, (struct sockaddr*)&m_address, sizeof(m_address));
	if (ret < 0) {
		close();
		return RESULT_ERR_GENERIC_IO;
	}
	return RESULT_OK;
}

void NetworkDevice::checkDevice()
{
	unsigned char value;
	ssize_t c = ::recv(m_fd, &value, 1, MSG_PEEK | MSG_DONTWAIT);
	if (c == 0 || (c < 0 && errno != EAGAIN)) {
		close();
	}
}
