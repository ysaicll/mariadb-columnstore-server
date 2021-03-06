/*****************************************************************************

Copyright (C) 2013, 2017, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/

/******************************************************************//**
@file fil/fil0pagecompress.cc
Implementation for page compressed file spaces.

Created 11/12/2013 Jan Lindström jan.lindstrom@mariadb.com
Updated 14/02/2015
***********************************************************************/

#include "fil0fil.h"
#include "fil0pagecompress.h"

#include <debug_sync.h>
#include <my_dbug.h>

#include "mem0mem.h"
#include "hash0hash.h"
#include "os0file.h"
#include "mach0data.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "log0recv.h"
#include "fsp0fsp.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "dict0dict.h"
#include "page0page.h"
#include "page0zip.h"
#include "trx0sys.h"
#include "row0mysql.h"
#include "ha_prototypes.h"  // IB_LOG_
#include "buf0lru.h"
#include "ibuf0ibuf.h"
#include "sync0sync.h"
#include "zlib.h"
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#endif
#include "row0mysql.h"
#ifdef HAVE_LZ4
#include "lz4.h"
#endif
#ifdef HAVE_LZO
#include "lzo/lzo1x.h"
#endif
#ifdef HAVE_LZMA
#include "lzma.h"
#endif
#ifdef HAVE_BZIP2
#include "bzlib.h"
#endif
#ifdef HAVE_SNAPPY
#include "snappy-c.h"
#endif

/* Used for debugging */
//#define UNIV_PAGECOMPRESS_DEBUG 1

/****************************************************************//**
For page compressed pages compress the page before actual write
operation.
@return compressed page to be written*/
UNIV_INTERN
byte*
fil_compress_page(
/*==============*/
	fil_space_t*	space,	/*!< in,out: tablespace (NULL during IMPORT) */
	byte*	buf,		/*!< in: buffer from which to write; in aio
				this must be appropriately aligned */
	byte*	out_buf,	/*!< out: compressed buffer */
	ulint	len,		/*!< in: length of input buffer.*/
	ulint	level,		/* in: compression level */
	ulint	block_size,	/*!< in: block size */
	bool	encrypted,	/*!< in: is page also encrypted */
	ulint*	out_len)	/*!< out: actual length of compressed
				page */
{
	int err = Z_OK;
	int comp_level = int(level);
	ulint header_len = FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE;
	ulint write_size = 0;
#if HAVE_LZO
	lzo_uint write_size_lzo = write_size;
#endif
	/* Cache to avoid change during function execution */
	ulint comp_method = innodb_compression_algorithm;
	bool allocated = false;

	/* page_compression does not apply to tables or tablespaces
	that use ROW_FORMAT=COMPRESSED */
	ut_ad(!space || !FSP_FLAGS_GET_ZIP_SSIZE(space->flags));

	if (encrypted) {
		header_len += FIL_PAGE_COMPRESSION_METHOD_SIZE;
	}

	if (!out_buf) {
		allocated = true;
		ulint size = UNIV_PAGE_SIZE;

		/* Both snappy and lzo compression methods require that
		output buffer used for compression is bigger than input
		buffer. Increase the allocated buffer size accordingly. */
#if HAVE_SNAPPY
		if (comp_method == PAGE_SNAPPY_ALGORITHM) {
			size = snappy_max_compressed_length(size);
		}
#endif
#if HAVE_LZO
		if (comp_method == PAGE_LZO_ALGORITHM) {
			size += LZO1X_1_15_MEM_COMPRESS;
		}
#endif

		out_buf = static_cast<byte *>(ut_malloc_nokey(size));
	}

	ut_ad(buf);
	ut_ad(out_buf);
	ut_ad(len);
	ut_ad(out_len);

	/* Let's not compress file space header or
	extent descriptor */
	switch (fil_page_get_type(buf)) {
	case 0:
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_PAGE_COMPRESSED:
		*out_len = len;
		goto err_exit;
	}

	/* If no compression level was provided to this table, use system
	default level */
	if (comp_level == 0) {
		comp_level = page_zip_level;
	}

	DBUG_LOG("compress", "Preparing for space "
		 << (space ? space->id : 0) << " '"
		 << (space ? space->name : "(import)") << "' len " << len);

	write_size = UNIV_PAGE_SIZE - header_len;

	switch(comp_method) {
#ifdef HAVE_LZ4
	case PAGE_LZ4_ALGORITHM:

#ifdef HAVE_LZ4_COMPRESS_DEFAULT
		err = LZ4_compress_default((const char *)buf,
			(char *)out_buf+header_len, len, write_size);
#else
		err = LZ4_compress_limitedOutput((const char *)buf,
			(char *)out_buf+header_len, len, write_size);
#endif /* HAVE_LZ4_COMPRESS_DEFAULT */
		write_size = err;

		if (err == 0) {
			goto err_exit;
		}
		break;
#endif /* HAVE_LZ4 */
#ifdef HAVE_LZO
	case PAGE_LZO_ALGORITHM:
		err = lzo1x_1_15_compress(
			buf, len, out_buf+header_len, &write_size_lzo, out_buf+UNIV_PAGE_SIZE);

		write_size = write_size_lzo;

		if (err != LZO_E_OK || write_size > UNIV_PAGE_SIZE-header_len) {
			goto err_exit;
		}

		break;
#endif /* HAVE_LZO */
#ifdef HAVE_LZMA
	case PAGE_LZMA_ALGORITHM: {
		size_t out_pos=0;

		err = lzma_easy_buffer_encode(
			comp_level,
			LZMA_CHECK_NONE,
			NULL, 	/* No custom allocator, use malloc/free */
			reinterpret_cast<uint8_t*>(buf),
			len,
			reinterpret_cast<uint8_t*>(out_buf + header_len),
			&out_pos,
			(size_t)write_size);

		if (err != LZMA_OK || out_pos > UNIV_PAGE_SIZE-header_len) {
			write_size = out_pos;
			goto err_exit;
		}

		write_size = out_pos;

		break;
	}
#endif /* HAVE_LZMA */

#ifdef HAVE_BZIP2
	case PAGE_BZIP2_ALGORITHM: {

		err = BZ2_bzBuffToBuffCompress(
			(char *)(out_buf + header_len),
			(unsigned int *)&write_size,
			(char *)buf,
			len,
			1,
			0,
			0);

		if (err != BZ_OK || write_size > UNIV_PAGE_SIZE-header_len) {
			goto err_exit;
		}
		break;
	}
#endif /* HAVE_BZIP2 */

#ifdef HAVE_SNAPPY
	case PAGE_SNAPPY_ALGORITHM:
	{
		snappy_status cstatus;
		write_size = snappy_max_compressed_length(UNIV_PAGE_SIZE);

		cstatus = snappy_compress(
			(const char *)buf,
			(size_t)len,
			(char *)(out_buf+header_len),
			(size_t*)&write_size);

		if (cstatus != SNAPPY_OK || write_size > UNIV_PAGE_SIZE-header_len) {
			err = (int)cstatus;
			goto err_exit;
		}
		break;
	}
#endif /* HAVE_SNAPPY */

	case PAGE_ZLIB_ALGORITHM:
		err = compress2(out_buf+header_len, (ulong*)&write_size, buf,
				uLong(len), comp_level);

		if (err != Z_OK) {
			goto err_exit;
		}
		break;

	case PAGE_UNCOMPRESSED:
		*out_len = len;
		return (buf);
		break;
	default:
		ut_error;
		break;
	}

	/* Set up the page header */
	memcpy(out_buf, buf, FIL_PAGE_DATA);
	/* Set up the checksum */
	mach_write_to_4(out_buf+FIL_PAGE_SPACE_OR_CHKSUM, BUF_NO_CHECKSUM_MAGIC);

	/* Set up the compression algorithm */
	mach_write_to_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, comp_method);

	if (encrypted) {
		/* Set up the correct page type */
		mach_write_to_2(out_buf+FIL_PAGE_TYPE, FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
		mach_write_to_2(out_buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE, comp_method);
	} else {
		/* Set up the correct page type */
		mach_write_to_2(out_buf+FIL_PAGE_TYPE, FIL_PAGE_PAGE_COMPRESSED);
	}

	/* Set up the actual payload lenght */
	mach_write_to_2(out_buf+FIL_PAGE_DATA, write_size);

#ifdef UNIV_DEBUG
	/* Verify */
	ut_ad(fil_page_is_compressed(out_buf) || fil_page_is_compressed_encrypted(out_buf));
	ut_ad(mach_read_from_4(out_buf+FIL_PAGE_SPACE_OR_CHKSUM) == BUF_NO_CHECKSUM_MAGIC);
	ut_ad(mach_read_from_2(out_buf+FIL_PAGE_DATA) == write_size);
	ut_ad(mach_read_from_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION) == (ulint)comp_method ||
		mach_read_from_2(out_buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE) == (ulint)comp_method);

	/* Verify that page can be decompressed */
	{
		byte *comp_page;
		byte *uncomp_page;

		comp_page = static_cast<byte *>(ut_malloc_nokey(UNIV_PAGE_SIZE));
		uncomp_page = static_cast<byte *>(ut_malloc_nokey(UNIV_PAGE_SIZE));
		memcpy(comp_page, out_buf, UNIV_PAGE_SIZE);

		fil_decompress_page(uncomp_page, comp_page, ulong(len), NULL);

		if (buf_page_is_corrupted(false, uncomp_page, univ_page_size,
					  space)) {
			buf_page_print(uncomp_page, univ_page_size);
			ut_ad(0);
		}

		ut_free(comp_page);
		ut_free(uncomp_page);
	}
#endif /* UNIV_DEBUG */

	write_size+=header_len;

	if (block_size <= 0) {
		block_size = 512;
	}

	ut_ad(write_size > 0 && block_size > 0);

	/* Actual write needs to be alligned on block size */
	if (write_size % block_size) {
		size_t tmp = write_size;
		write_size =  (size_t)ut_uint64_align_up((ib_uint64_t)write_size, block_size);
		/* Clean up the end of buffer */
		memset(out_buf+tmp, 0, write_size - tmp);
#ifdef UNIV_DEBUG
		ut_a(write_size > 0 && ((write_size % block_size) == 0));
		ut_a(write_size >= tmp);
#endif
	}

	DBUG_LOG("compress", "Succeeded for space "
		 << (space ? space->id : 0) << " '"
		 << (space ? space->name : "(import)")
		 << "' len " << len << " out_len " << write_size);

	srv_stats.page_compression_saved.add((len - write_size));
	srv_stats.pages_page_compressed.inc();

	/* If we do not persistently trim rest of page, we need to write it
	all */
	if (!srv_use_trim) {
		memset(out_buf+write_size,0,len-write_size);
		write_size = len;
	}

	*out_len = write_size;

	if (allocated) {
		/* TODO: reduce number of memcpy's */
		memcpy(buf, out_buf, len);
		goto exit_free;
	} else {
		return(out_buf);
	}

err_exit:
	/* If error we leave the actual page as it was */

#ifndef UNIV_PAGECOMPRESS_DEBUG
	if (space && !space->printed_compression_failure) {
		space->printed_compression_failure = true;
#endif
		ib::warn() << "Compression failed for space: "
			   << space->id << " name: "
			   << space->name << " len: "
			   << len << " err: " << err << " write_size: "
			   << write_size
			   << " compression method: "
			   << fil_get_compression_alg_name(comp_method)
			   << ".";
#ifndef UNIV_PAGECOMPRESS_DEBUG
	}
#endif
	srv_stats.pages_page_compression_error.inc();
	*out_len = len;

exit_free:
	if (allocated) {
		ut_free(out_buf);
	}

	return (buf);

}

/****************************************************************//**
For page compressed pages decompress the page after actual read
operation. */
UNIV_INTERN
void
fil_decompress_page(
/*================*/
	byte*	page_buf,	/*!< in: preallocated buffer or NULL */
	byte*	buf,		/*!< out: buffer from which to read; in aio
				this must be appropriately aligned */
	ulong	len,		/*!< in: length of output buffer.*/
	ulint*	write_size,	/*!< in/out: Actual payload size of
				the compressed data. */
	bool	return_error)	/*!< in: true if only an error should
				be produced when decompression fails.
				By default this parameter is false. */
{
	int err = 0;
	ulint actual_size = 0;
	ib_uint64_t compression_alg = 0;
	byte *in_buf;
	ulint ptype;
	ulint header_len;

	ut_ad(buf);
	ut_ad(len);

	ptype = mach_read_from_2(buf+FIL_PAGE_TYPE);

	switch (ptype) {
	case FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED:
		header_len = FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE
			+ FIL_PAGE_COMPRESSION_METHOD_SIZE;
		break;
	case FIL_PAGE_PAGE_COMPRESSED:
		header_len = FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE;
		break;
	default:
		/* The page is not in our format. */
		return;
	}

	// If no buffer was given, we need to allocate temporal buffer
	if (page_buf == NULL) {
		in_buf = static_cast<byte *>(ut_malloc_nokey(UNIV_PAGE_SIZE));
		memset(in_buf, 0, UNIV_PAGE_SIZE);
	} else {
		in_buf = page_buf;
	}

	/* Before actual decompress, make sure that page type is correct */

	if (mach_read_from_4(buf+FIL_PAGE_SPACE_OR_CHKSUM)
	    != BUF_NO_CHECKSUM_MAGIC
	    || (ptype != FIL_PAGE_PAGE_COMPRESSED
		&& ptype != FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED)) {
		ib::error() << "Corruption: We try to uncompress corrupted "
			"page CRC "
			<< mach_read_from_4(buf+FIL_PAGE_SPACE_OR_CHKSUM)
			<< " type " << ptype  << " len " << len << ".";

		if (return_error) {
			goto error_return;
		}
		ut_error;
	}

	/* Get compression algorithm */
	if (ptype == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		compression_alg = static_cast<ib_uint64_t>(mach_read_from_2(buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE));
	} else {
		compression_alg = mach_read_from_8(buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	}

	/* Get the actual size of compressed page */
	actual_size = mach_read_from_2(buf+FIL_PAGE_DATA);
	/* Check if payload size is corrupted */
	if (actual_size == 0 || actual_size > UNIV_PAGE_SIZE) {
		ib::error() << "Corruption: We try to uncompress corrupted page"
			    << " actual size: " << actual_size
			    << " compression method: "
			    << fil_get_compression_alg_name(compression_alg)
			    << ".";
		if (return_error) {
			goto error_return;
		}
		ut_error;
	}

	/* Store actual payload size of the compressed data. This pointer
	points to buffer pool. */
	if (write_size) {
		*write_size = actual_size;
	}

	DBUG_LOG("compress", "Preparing for decompress for len "
		 << actual_size << ".");

	switch(compression_alg) {
	case PAGE_ZLIB_ALGORITHM:
		err= uncompress(in_buf, &len, buf+header_len, (unsigned long)actual_size);

		/* If uncompress fails it means that page is corrupted */
		if (err != Z_OK) {
			goto err_exit;
			if (return_error) {
				goto error_return;
			}
		}
		break;

#ifdef HAVE_LZ4
	case PAGE_LZ4_ALGORITHM:
		err = LZ4_decompress_fast((const char *)buf+header_len, (char *)in_buf, len);

		if (err != (int)actual_size) {
			goto err_exit;
			if (return_error) {
				goto error_return;
			}
		}
		break;
#endif /* HAVE_LZ4 */
#ifdef HAVE_LZO
	case PAGE_LZO_ALGORITHM: {
		ulint olen = 0;
		lzo_uint olen_lzo = olen;
		err = lzo1x_decompress((const unsigned char *)buf+header_len,
			actual_size,(unsigned char *)in_buf, &olen_lzo, NULL);

		olen = olen_lzo;

		if (err != LZO_E_OK || (olen == 0 || olen > UNIV_PAGE_SIZE)) {
			len = olen;
			goto err_exit;
			if (return_error) {
				goto error_return;
			}
		}
		break;
        }
#endif /* HAVE_LZO */
#ifdef HAVE_LZMA
	case PAGE_LZMA_ALGORITHM: {

		lzma_ret	ret;
		size_t		src_pos = 0;
		size_t		dst_pos = 0;
		uint64_t 	memlimit = UINT64_MAX;

		ret = lzma_stream_buffer_decode(
			&memlimit,
			0,
			NULL,
			buf+header_len,
			&src_pos,
			actual_size,
			in_buf,
			&dst_pos,
			len);


		if (ret != LZMA_OK || (dst_pos == 0 || dst_pos > UNIV_PAGE_SIZE)) {
			len = dst_pos;
			goto err_exit;
			if (return_error) {
				goto error_return;
			}
		}

		break;
	}
#endif /* HAVE_LZMA */
#ifdef HAVE_BZIP2
	case PAGE_BZIP2_ALGORITHM: {
		unsigned int dst_pos = UNIV_PAGE_SIZE;

		err = BZ2_bzBuffToBuffDecompress(
			(char *)in_buf,
			&dst_pos,
			(char *)(buf+header_len),
			actual_size,
			1,
			0);

		if (err != BZ_OK || (dst_pos == 0 || dst_pos > UNIV_PAGE_SIZE)) {
			len = dst_pos;
			goto err_exit;
			if (return_error) {
				goto error_return;
			}
		}
		break;
	}
#endif /* HAVE_BZIP2 */
#ifdef HAVE_SNAPPY
	case PAGE_SNAPPY_ALGORITHM:
	{
		snappy_status cstatus;
		ulint olen = UNIV_PAGE_SIZE;

		cstatus = snappy_uncompress(
			(const char *)(buf+header_len),
			(size_t)actual_size,
			(char *)in_buf,
			(size_t*)&olen);

		if (cstatus != SNAPPY_OK || (olen == 0 || olen > UNIV_PAGE_SIZE)) {
			err = (int)cstatus;
			len = olen;
			goto err_exit;
			if (return_error) {
				goto error_return;
			}
		}

		break;
	}
#endif /* HAVE_SNAPPY */
	default:
		goto err_exit;
		if (return_error) {
			goto error_return;
		}
		break;
	}

	srv_stats.pages_page_decompressed.inc();

	/* Copy the uncompressed page to the buffer pool, not
	really any other options. */
	memcpy(buf, in_buf, len);

error_return:
	if (page_buf != in_buf) {
		ut_free(in_buf);
	}

	return;

err_exit:
	/* Note that as we have found the page is corrupted, so
	all this could be incorrect. */
	ulint space_id = mach_read_from_4(buf+FIL_PAGE_SPACE_ID);
	fil_space_t* space = fil_space_acquire_for_io(space_id);

	ib::error() << "Corruption: Page is marked as compressed"
		    << " space: " <<  space_id << " name: "
		    << (space ? space->name : "NULL")
		    << " but uncompress failed with error: " << err
		    << " size: " << actual_size
		    << " len: " << len
		    << " compression method: "
		    << fil_get_compression_alg_name(compression_alg) << ".";

	buf_page_print(buf, univ_page_size);
	fil_space_release_for_io(space);
	ut_ad(0);
}
