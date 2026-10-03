/*
WEB_MAPS_BACKEND.CPP

The server's maps as a read-only WasmFS directory (mounted at /data/maps by
web_platform.c). The directory lists maps/index.json; each file reads the
server's maps/<name> in HTTP ranges as the game asks, through a read-ahead
window of its own, and keeps nothing else (a map is up to 175 MB, and the
game copies the ones it plays into its cache).

The requests are synchronous: WasmFS reads on the calling thread, which is
one of the game's (workers, where a synchronous request may block).
*/

#include <emscripten.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend.h"
#include "file.h"
#include "memory_backend.h"
#include "wasmfs.h"

namespace {

/* bytes read ahead of each request, and attempts at each (a phone's
network drops some) */
constexpr size_t READ_AHEAD_SIZE = 4 * 1024 * 1024;
constexpr int MAXIMUM_ATTEMPTS = 4;

/* fills buffer with "name size\n" for each map; returns its length, or -1 */
EM_JS(int, maps_fetch_list, (char *buffer, int size), {
	try {
		var request = new XMLHttpRequest();
		request.open('GET', 'maps/index.json', false);
		request.send(null);
		if (request.status != 200) return -1;
		var list = JSON.parse(request.responseText);
		var text = '';
		for (var index = 0; index < list.files.length; index++)
			text += list.files[index].name + ' ' + list.files[index].size + '\n';
		return stringToUTF8(text, buffer, size);
	} catch (exception) {
		console.error('the list of maps', exception);
		return -1;
	}
});

/* reads length bytes at offset of the server's map into buffer; returns
the bytes read, or -1 */
EM_JS(int, maps_fetch_range, (const char *name, double offset, int length, uint8_t *buffer), {
	var url = 'maps/' + encodeURIComponent(UTF8ToString(name));
	try {
		var request = new XMLHttpRequest();
		request.open('GET', url, false);
		request.responseType = 'arraybuffer';
		request.setRequestHeader('Range', 'bytes=' + offset + '-' + (offset + length - 1));
		request.send(null);
		if (request.status != 206 && request.status != 200) return -1;
		var bytes = new Uint8Array(request.response);
		// a server that ignores ranges sends the whole file
		if (request.status == 200) bytes = bytes.subarray(offset, offset + length);
		var count = Math.min(bytes.length, length);
		HEAPU8.set(bytes.subarray(0, count), buffer);
		return count;
	} catch (exception) {
		console.error('reading', url, exception);
		return -1;
	}
});

class MapFile : public wasmfs::DataFile {
	std::string name;
	off_t size;
	std::vector<uint8_t> window;
	off_t window_offset = 0;
	size_t window_length = 0;

	bool fetch(off_t offset, size_t length, uint8_t *buffer)
	{
		for (int attempt = 0; attempt < MAXIMUM_ATTEMPTS; attempt++)
		{
			int count = maps_fetch_range(name.c_str(), (double)offset, (int)length, buffer);

			if (count == (int)length)
				return true;
		}
		std::fprintf(stderr, "cannot read %zu bytes at %lld of the server's %s\n", length, (long long)offset,
			name.c_str());
		return false;
	}

	int open(wasmfs::oflags_t) override { return 0; }
	int close() override { return 0; }
	int flush() override { return 0; }
	off_t getSize() override { return size; }
	int setSize(off_t) override { return -EACCES; }
	ssize_t write(const uint8_t *, size_t, off_t) override { return -EACCES; }

	ssize_t read(uint8_t *buffer, size_t length, off_t offset) override
	{
		size_t done = 0;

		if (offset >= size)
			return 0;
		if ((off_t)length > size - offset)
			length = (size_t)(size - offset);
		while (done < length)
		{
			off_t at = offset + (off_t)done;
			size_t wanted = length - done;

			if (at >= window_offset && at < window_offset + (off_t)window_length)
			{
				size_t count = std::min(wanted, (size_t)(window_offset + (off_t)window_length - at));

				std::memcpy(buffer + done, window.data() + (at - window_offset), count);
				done += count;
				continue;
			}
			if (wanted >= READ_AHEAD_SIZE)
			{
				/* a large read goes straight to the caller */
				size_t count = wanted - wanted % READ_AHEAD_SIZE;

				if (!fetch(at, count, buffer + done))
					return done ? (ssize_t)done : -EIO;
				done += count;
				continue;
			}
			window_length = (size_t)std::min((off_t)READ_AHEAD_SIZE, size - at);
			window.resize(READ_AHEAD_SIZE);
			if (!fetch(at, window_length, window.data()))
			{
				window_length = 0;
				return done ? (ssize_t)done : -EIO;
			}
			window_offset = at;
		}
		return (ssize_t)done;
	}

public:
	MapFile(wasmfs::backend_t backend, std::string name, off_t size)
		: DataFile(0444, backend), name(std::move(name)), size(size) {}
};

/* the maps the server lists */
class MapsDirectory : public wasmfs::MemoryDirectory {
public:
	MapsDirectory(mode_t mode, wasmfs::backend_t backend) : MemoryDirectory(mode, backend)
	{
		std::vector<char> list(64 * 1024);
		int length = maps_fetch_list(list.data(), (int)list.size());
		const char *cursor = list.data();
		int count = 0;

		if (length < 0)
		{
			std::fprintf(stderr, "the server lists no maps (maps/index.json)\n");
			return;
		}
		while (*cursor)
		{
			char name[256];
			long long file_size;
			int consumed;

			if (std::sscanf(cursor, "%255s %lld\n%n", name, &file_size, &consumed) != 2)
				break;
			insertChild(name, std::make_shared<MapFile>(backend, name, (off_t)file_size));
			cursor += consumed;
			count++;
		}
		std::printf("the server has %d maps\n", count);
	}
};

class MapsBackend : public wasmfs::Backend {
public:
	std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override
	{
		return std::make_shared<wasmfs::MemoryDataFile>(mode, this);
	}
	std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override
	{
		return std::make_shared<MapsDirectory>(mode, this);
	}
	std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override
	{
		return std::make_shared<wasmfs::MemorySymlink>(target, this);
	}
};

} // namespace

extern "C" wasmfs::backend_t web_maps_backend_create(void)
{
	return wasmfs::wasmFS.addBackend(std::make_unique<MapsBackend>());
}
