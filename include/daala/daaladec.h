/*Daala video codec
Copyright (c) 2006-2013 Daala project contributors.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

/**\file
 * The <tt>libdaala</tt> C decoding API.*/
#if !defined(_daala_daaladec_H)
# define _daala_daaladec_H (1)
# include "codec.h"

# if defined(__cplusplus)
extern "C" {
# endif
# if OD_GNUC_PREREQ(4, 0)
#  pragma GCC visibility push(default)
# endif

/**\name Decoder state
   The following data structures are opaque, and their contents are not
    publicly defined by this API.
   Referring to their internals directly is unsupported, and may break without
    warning.*/
/*@{*/
/**The decoder context.*/
typedef struct daala_dec_ctx daala_dec_ctx;
/**Setup information.
   This contains auxiliary information decoded from the setup header by
    daala_decode_headerin() to be passed to daala_decode_alloc().
   It can be re-used to initialize any number of decoders, and can be freed
    via daala_setup_free() at any time.*/
typedef struct daala_setup_info daala_setup_info;
/*@}*/

/**\defgroup decfuncs Functions for Decoding*/
/*@{*/
/**\name Functions for decoding
 * You must link to <tt>libdaalaadec</tt> if you use any of the functions in
 *  this section.
 **/
/*@{*/

/**Allocates a decoder instance.
 * \param info A #daala_info struct filled via daala_decode_headerin().
 * \param setup A #daala_setup_info handle returned via
 *               daala_decode_headerin().
 * \return The initialized #daala_dec_ctx handle.
 * \retval NULL If the decoding parameters were invalid.*/
extern daala_dec_ctx *daala_decode_alloc(const daala_info *info,
 const daala_setup_info *setup);
/**Releases all storage used for the decoder setup information.
 * This should be called after you no longer want to create any decoders for
 *  a stream whose headers you have parsed with th_decode_headerin().
 * \param setup The setup information to free.
 *              This can safely be <tt>NULL</tt>.*/
extern void daala_setup_free(daala_setup_info *setup);
/**Decoder control function.
 * This is used to provide advanced control of the decoding process.
 * \param dec A #daala_dec_ctx handle.
 * \param req The control code to process.
 *            See \ref decctlcodes "the list of available control codes"
 *             for details.
 * \param buf The parameters for this control code.
 * \param buf_sz The size of the parameter buffer.*/
extern int daala_decode_ctl(daala_dec_ctx *dec,
 int req, void *buf, size_t buf_sz);
/**Frees an allocated decoder instance.
 * \param dec A #daala_dec_ctx handle.*/
extern void daala_decode_free(daala_dec_ctx *dec);
/**Retrieves decoded video data frames.
 * \param dec A #daala_dec_ctx handle.
 * \param img A buffer to receive the decoded image data.*/
extern int daala_decode_img_out(daala_dec_ctx *dec,od_img *img);
/*@}*/
/*@}*/

# if OD_GNUC_PREREQ(4, 0)
#  pragma GCC visibility pop
# endif
# if defined(__cplusplus)
}
# endif

#endif
