#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

/*
 * Compile with
 * cc -o dump_offsets -Wall -Werror -ansi -std=c89 dump_offsets.c
 */
extern int	 errno;

#define RECORD_SIGNATURE	"FILE"
#define RECORD_LENGTH		1024
#define OUTPUT_DIR		"dump"

#define ATTR_FIXED_HEADER_SIZE		0x10
#define ATTR_FILENAME_LENGTH_OFFSET	(ATTR_FIXED_HEADER_SIZE + 0x48)
#define ATTR_FILENAME_NAME_OFFSET	(ATTR_FIXED_HEADER_SIZE + 0x4a)


const char *
get_record_name_by_id(uint32_t record_id) {
    /* Return the name of well-known record numbers */
    switch (record_id) {
        case 0:
            return "$MFT";
        case 1:
            return "$MFTMirr";
        case 2:
            return "$LogFile";
        case 3:
            return "$Volume";
        case 4:
            return "$AttrDef";
        case 5:
            return "$. (root dir)";
        case 6:
            return "$Bitmap";
        case 7:
            return "$Boot";
        case 8:
            return "$BadClus";
        case 9:
            return "$Secure";
        case 10:
            return "$UpCase";
        case 11:
            return "$Extend";
        default:
            return "";
    }
}


const wchar_t *
extract_name_from_record(char *record) {
	/* Return the file name of a file record, NULL otherwise.
	 * Note: this function is very hacky, there is no parsing of the
	 *       record structure, just jumping around at semi-fixed offsets.
	 *
	 * The returned pointer, if not NULL, must be freed
	 */
	wchar_t		*file_name;
	size_t		 idx;
	uint32_t	 attr_type,
			 attr_len;
	uint16_t	 offset,
			 utf16_char;
	unsigned char	 file_name_size;

	/* record is expected to be a 1024-byte string */
	if (strncmp(record, RECORD_SIGNATURE, strlen(RECORD_SIGNATURE)) != 0) {
		/* not an MFT record */
		return(NULL);
	}
	/* locate the attribute list start */
	offset = *(&record[0x14]);

	file_name = NULL;
	for (;;) {
		if (offset >= 1024) {
			/* if the record is well formed, should not reach */
			break;
		}
		attr_type = *(&record[offset]);
		attr_len = *(uint32_t *)(&record[offset + 4]);
		/* check if there are more attributes */
		if (attr_type == 0xffffffff) {
			/* end of attributes list reached */
			break;
		}
		/* look for FILE_NAME attribute */
		if (attr_type != 0x30) {
			/* not a $FILE_NAME attribute */
			offset += attr_len;
			/* align to the next 8-byte boundary */
			offset += (8 - (offset % 8)) & 0x7;
			continue;
		}
		/* allocate space for the file name and copy the content */
		file_name_size = *(&record[offset + ATTR_FILENAME_LENGTH_OFFSET]);
		if ((file_name = malloc(
				(file_name_size + 1) * sizeof(wchar_t))
				) == NULL) {
			/* WTF */
			abort();
		}
		/* UTF-16 to wchar_t. Microsoft, I hate you */
		for (idx = 0; idx < file_name_size; idx++) {
			utf16_char = *(&record[offset + ATTR_FILENAME_NAME_OFFSET + idx * 2]);
			file_name[idx] = utf16_char;
		}
		file_name[file_name_size] = L'\0';
		break;
	}
	return((file_name));
}

static
void
usage(const char *prog) {
	fprintf(stderr, "This program dumps all the MFT records of a NTFS "
			"partition dump.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Usage: %s <NTFS partition image> [--dump]\n", prog);
}


int
dump_to_file(const char *data, size_t size, off_t offset) {
	FILE	*fp;
	char	 outfile[PATH_MAX];

	snprintf(outfile, PATH_MAX - 1, "%s/record_%016llx", OUTPUT_DIR, offset);
	outfile[PATH_MAX-1] = '\0';
	if ((fp = fopen(outfile, "wb")) == NULL)
		return(0);
	if (fwrite(data, size, 1, fp) != 1)
		return(0);
	fclose(fp);
	return(1);
}


int
main(int argc, char *argv[]) {
	FILE		*fp;
	off_t		 offset;
	wchar_t		*file_name;
	char		 chunk[RECORD_LENGTH],
			*record_name,
			*in_use,
			*type;
	unsigned int	 idx;
	uint32_t	 record_num;
	uint16_t	 flags;
	short		 do_dump;

	if (argc < 2) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	if (argc == 3 && strcmp(argv[2], "--dump") == 0)
		do_dump = 1;
	else
		do_dump = 0;

	if ((fp = fopen(argv[1], "r")) == NULL) {
		fprintf(stderr, "Error: cannot open file '%s': %s",argv[1],
				strerror(errno));
	}

	for(idx = 0;;) {
		offset = ftell(fp);
		if (fread(chunk, RECORD_LENGTH, 1, fp) != 1) {
			if (!feof(fp)) {
				fprintf(stderr, "Error: fread failed: %s\n",
						strerror(errno));
			}
			break;
		}
		if (strncmp(chunk, RECORD_SIGNATURE,
				strlen(RECORD_SIGNATURE)) == 0) {
			/* this is probably an MFT record */
			record_num = *(&chunk[0x2c]);
			record_name = (char *)get_record_name_by_id(record_num);
			file_name = (wchar_t *)extract_name_from_record(chunk);
			flags = *(&chunk[0x16]);
			if (flags & 0x1)
				in_use = "yes";
			else
				in_use = "no";
			if (flags & 0x2)
				type = "directory";
			else
				type = "file";
			wprintf(
				L"%u) Possible record found at "
				L"0x%016llx (id = %u , name = %ls, "
				L"type = %s, in use = %s)\n",
				idx,
				offset,
				record_num,
				file_name,
/*				(record_name[0] == '\0' ? "unknown" : record_name), */
				type,
				in_use
			);
			free(file_name);
			if (do_dump) {
				if (!dump_to_file(chunk, RECORD_LENGTH, offset)) {
					fprintf(stderr,
						"Error: dump_to_file: %s\n",
						strerror(errno));
					break;
				}
			}
			idx++;
		}
	}

	if (fp != NULL) {
		fclose(fp);
		fp = NULL;
	}

	exit(EXIT_SUCCESS);
}
