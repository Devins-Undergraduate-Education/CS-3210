#include "fcntl.h"
#include "fs.h"
#include "sched.h"
#include "stat.h"
#include "types.h"
#include "user.h"

#define MAX_LENGTH 15
#define MAX_LOOKBEHIND 4096
#define BUFFER_SIZE 4096

struct lz77_tuple {
	ushort match_distance;
	uchar match_length;
	uchar next_character;
};

uint lz77_compress(uchar *uncompressed_text, uint uncompressed_size,
		   uchar *compressed_text)
{
	uchar ptr_length, temp_ptr_length;
	ushort pos, temp_pos, output_ptr;
	uint compressed_ptr, output_size, look_behind, look_ahead;

	struct lz77_tuple output_tuple;

	// write size into first 4 bytes
	*((uint *)compressed_text) = uncompressed_size;
	compressed_ptr = output_size = 4;

	for (uint c = 0; c < uncompressed_size; ++c) {
		pos = 0;
		ptr_length = 0;
		for (temp_pos = 1;
		     (temp_pos < MAX_LOOKBEHIND) && (temp_pos <= c);
		     ++temp_pos) {
			look_behind = c - temp_pos;
			look_ahead = c;
			for (temp_ptr_length = 0;
			     uncompressed_text[look_ahead++] ==
			     uncompressed_text[look_behind++];
			     ++temp_ptr_length)
				if (temp_ptr_length == MAX_LENGTH)
					break;
			if (temp_ptr_length > ptr_length) {
				pos = temp_pos;
				ptr_length = temp_ptr_length;
				if (ptr_length == MAX_LENGTH)
					break;
			}
		}
		c += ptr_length;
		if (ptr_length && (c == uncompressed_size)) {
			output_tuple.match_length = ptr_length - 1;
			output_tuple.match_distance = pos;
			output_tuple.next_character =
				*(uncompressed_text + c - 1);
		} else {
			output_tuple.match_length = ptr_length;
			output_tuple.match_distance = pos;
			output_tuple.next_character = *(uncompressed_text + c);
		}
		(void)output_ptr;
		memmove(compressed_text + compressed_ptr, &(output_tuple),
			sizeof(struct lz77_tuple));
		compressed_ptr += sizeof(struct lz77_tuple);
	}

	return compressed_ptr;
}

uint lz77_decompress(uchar *compressed_text, uchar *uncompressed_text)
{
	uchar ptr_length;
	ushort pos;

	uint uncompressed_size = *((uint *)compressed_text);
	uint ptr_offset;

	struct lz77_tuple *input_tuple =
		(struct lz77_tuple *)(compressed_text + 4);

	for (uint c = 0; c < uncompressed_size; ++c) {
		pos = input_tuple->match_distance;
		ptr_length = input_tuple->match_length;
		if (pos)
			for (ptr_offset = c - pos;
			     ptr_length > 0 && ptr_offset < uncompressed_size;
			     --ptr_length) {
				uncompressed_text[c++] =
					uncompressed_text[ptr_offset++];
			}
		uncompressed_text[c] = input_tuple->next_character;
		input_tuple += 1;
	}

	return uncompressed_size;
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		printf(2, "Usage: %s <filename>\n", argv[0]);
		return 1;
	}

	struct stat st;
	char filename[100];
	strcpy(filename, argv[1]);

	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		printf(2, "Error opening file");
		exit();
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		exit();
	}

	int original_size = st.size;
	char *original_file = malloc(original_size);
	char *compressed_file = malloc(original_size * 2);
	char *decompressed_file = malloc(original_size);
	char *buffer = malloc(BUFFER_SIZE);

	uint buffer_ptr = 0;
	uint nbytes;
	while ((nbytes = read(fd, buffer, BUFFER_SIZE)) > 0) {
		memmove(original_file + buffer_ptr, buffer, nbytes);
		buffer_ptr += nbytes;
		if (buffer_ptr > original_size) {
			printf(2, "overshot\n");
			exit();
		}
	}

	close(fd);

	uint compressed_size = lz77_compress((uchar *)original_file,
					     (uint)original_size,
					     (uchar *)compressed_file);
	lz77_decompress((uchar *)compressed_file, (uchar *)decompressed_file);

	(void)compressed_size;

	if (compressed_size > original_size) {
		printf(2, "compressed size is greater than original size\n");
		exit();
	}

	for (int c = 0; c < buffer_ptr; ++c) {
		if (original_file[c] != decompressed_file[c]) {
			printf(2, "compression/decompression mismatch\n");
			exit();
		}
	}

	free(original_file);
	free(compressed_file);
	free(decompressed_file);

	exit();
}
