"""
Python API to Pegasus Notetaker devices.

Currently only Pegasus Mobile Notetaker `M210`_ is supported.

.. _M210: http://www.pegatech.com/?CategoryID=207&ArticleID=268

"""

from __future__ import absolute_import

import os
import select
import struct

import noter.daemon.hidraw
import noter.daemon.collections

__all__ =  [
    "CommunicationError",
    "TimeoutError",
    "M210",
    ]

class CommunicationError(Exception):
    """Raised when an unexpected message is received."""
    pass

class TimeoutError(Exception):
    """Raised when communication timeouts."""
    pass

class M210(object):
    """
    M210 exposes two interfaces via USB-connection. By default, udev
    creates two hidraw devices to represent these interfaces when the
    device is plugged in. Paths to these devices must be passed to
    initialize a M210-connection. Interface 1 is used for reading and
    writing, interface 2 only for reading.

    Usage example::

      >>> import pegatech
      >>> m210 = pegatech.M210(("/dev/hidraw1", "/dev/hidraw2"))
      >>> m210.get_info()
      {'download_size': 1364, 'firmware_version': 337, 'analog_version': 265, 'pad_version': 32028, 'mode': 2}
      >>> download_destination = open("m210notes", "wb")
      >>> m210.download_notes_to(download_destination)
      1364
      >>> download_destination.tell()
      1364
      >>> m210.delete_notes()
      >>> m210.get_info()
      {'download_size': 0, 'firmware_version': 337, 'analog_version': 265, 'pad_version': 32028, 'mode': 2}
    
    """

    _PACKET_PAYLOAD_SIZE = 62

    def __init__(self, hidraw_filepaths, read_timeout=1.0):
        self.read_timeout = read_timeout
        self._fds = []
        for filepath, mode in zip(hidraw_filepaths, (os.O_RDWR, os.O_RDONLY)):
            fd = os.open(filepath, mode)
            devinfo = noter.daemon.hidraw.get_devinfo(fd)
            if devinfo != {'product': 257, 'vendor': 3616, 'bustype': 3}:
                raise ValueError('%s is not a M210 hidraw device.' % filepath)
            self._fds.append(fd)

    def _read(self, iface_n):
        fd = self._fds[iface_n]

        rlist, _, _ = select.select([fd], [], [], self.read_timeout)
        if not fd in rlist:
            raise TimeoutError("Reading timeouted.")

        response_size = (64, 9)[iface_n]

        while True:
            response = os.read(fd, response_size)

            if iface_n == 0:
                if response[:2] == '\x80\xb5':
                    continue # Ignore mode button events, at least for now.
            break

        return response

    def _write(self, request):
        request_header = struct.pack('BBB', 0x00, 0x02, len(request))
        os.write(self._fds[0], request_header + request)

    def _wait_ready(self):
        while True:
            self._write('\x95')
            try:
                response = self._read(0)
            except TimeoutError:
                continue
            break

        if (response[:3] != '\x80\xa9\x28'
            or response[9] != '\x0e'):
            raise CommunicationError('Unexpected response to info request: %s'
                                     % response)

        firmware_ver, analog_ver, pad_ver = struct.unpack('>HHH', response[3:9])

        return {'firmware_version': firmware_ver,
                'analog_version': analog_ver,
                'pad_version': pad_ver,
                'mode': ord(response[10])}

    def _accept_upload(self):
        self._write('\xb6')

    def _reject_upload(self):
        self._write('\xb7')

    def _begin_upload(self):
        """Return packet count."""

        self._write('\xb5')
        try:
            try:
                response = self._read(0)
            except TimeoutError:
                # M210 with zero notes stored in it does not send any response.
                return 0
            if (not response.startswith('\xaa\xaa\xaa\xaa\xaa')
                or response[7:9] != '\x55\x55'):
                raise CommunicationError("Unrecognized upload response: %s"
                                         % response[:9])
            return struct.unpack('>H', response[5:7])[0]
        except Exception, e:
            # If just anything goes wrong, try to leave the device in
            # a decent state by sending a reject request. Then just
            # raise the original exception.
            try:
                self._reject_upload()
            except:
                pass
            raise e

    def get_info(self):
        """Return a dict containing information about versions, current mode
        and the size of stored notes in bytes.
        """

        info = self._wait_ready()
        packet_count = self._begin_upload()
        self._reject_upload()
        info['download_size'] = packet_count * M210._PACKET_PAYLOAD_SIZE
        return info

    def delete_notes(self):
        """Delete all notes stored in the device."""

        self._wait_ready()
        self._write('\xb0')

    def _receive_packet(self):
        response = self._read(0)
        packet_number = struct.unpack('>H', response[:2])[0]
        return packet_number, response[2:]

    def download_notes_to(self, destination_file):
        """Download notes to an open `destination_file` in one pass.

        Return the total size (bytes) of downloaded notes.
        """

        def request_lost_packets(lost_packet_numbers):
            while lost_packet_numbers:
                lost_packet_number = lost_packet_numbers[0]
                self._write('\xb7' + struct.pack('>H', lost_packet_number))
                packet_number, packet_payload = self._receive_packet()
                if packet_number == lost_packet_number:
                    lost_packet_numbers.remove(lost_packet_number)
                    destination_file.write(packet_payload)

        # Wait until the device is ready to upload packages to
        # us. This is needed because previous public API call might
        # have failed and the device hasn't had enough time to recover
        # from that incident yet.
        self._wait_ready()

        packet_count = self._begin_upload()
        if packet_count == 0:
            # The device does not have any notes, inform that it's ok
            # and let it rest.
            self._reject_upload()
            return 0

        self._accept_upload()
        lost_packet_numbers = noter.daemon.collections.OrderedSet()

        # For some odd reason, packet numbering starts from 1 in
        # M210's memory.
        for expected_number in range(1, packet_count + 1):

            packet_number, packet_payload = self._receive_packet()

            if packet_number != expected_number:
                lost_packet_numbers.add(expected_number)

            if not lost_packet_numbers:
                # It's safe to write only when all expected packets so
                # far have been received. The behavior is changed as
                # soon as the first package is lost.
                destination_file.write(packet_payload)

        request_lost_packets(lost_packet_numbers)

        # Thank the device for cooperation and let it rest.
        self._accept_upload()

        return packet_count * M210._PACKET_PAYLOAD_SIZE
