/*
    JPEG XL (JXL) support for QImage.

    SPDX-FileCopyrightText: 2021 Daniel Novomesky <dnovomesky@gmail.com>

    SPDX-License-Identifier: BSD-2-Clause
*/

#include <QThread>
#include <QtGlobal>

#include "jxl_p.h"
#include "util_p.h"

#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

#if JPEGXL_NUMERIC_VERSION >= JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
#include <jxl/cms.h>
#endif

#include <string.h>

// Avoid rotation on buggy Qts (see also https://bugreports.qt.io/browse/QTBUG-126575)
#if (QT_VERSION >= QT_VERSION_CHECK(6, 5, 7) && QT_VERSION < QT_VERSION_CHECK(6, 6, 0)) || (QT_VERSION >= QT_VERSION_CHECK(6, 7, 3))
#ifndef JXL_QT_AUTOTRANSFORM
#define JXL_QT_AUTOTRANSFORM
#endif
#endif

#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
#ifndef JXL_HDR_PRESERVATION_DISABLED
// Define JXL_HDR_PRESERVATION_DISABLED to disable HDR preservation
// (HDR images are saved as UINT16).
#define JXL_HDR_PRESERVATION_DISABLED
#endif
#endif

#ifndef JXL_DECODE_BOXES_DISABLED
// Decode Boxes in order to read optional metadata (XMP, Exif, etc...).
// Define JXL_DECODE_BOXES_DISABLED to disable Boxes decoding.
// #define JXL_DECODE_BOXES_DISABLED
#endif

#define FEATURE_LEVEL_5_WIDTH 262144
#define FEATURE_LEVEL_5_HEIGHT 262144
#define FEATURE_LEVEL_5_PIXELS 268435456

#if QT_POINTER_SIZE < 8
#define MAX_IMAGE_WIDTH 32767
#define MAX_IMAGE_HEIGHT 32767
#define MAX_IMAGE_PIXELS FEATURE_LEVEL_5_PIXELS
#else // JXL code stream level 5
#define MAX_IMAGE_WIDTH FEATURE_LEVEL_5_WIDTH
#define MAX_IMAGE_HEIGHT FEATURE_LEVEL_5_HEIGHT
#define MAX_IMAGE_PIXELS FEATURE_LEVEL_5_PIXELS
#endif

QJpegXLHandler::QJpegXLHandler()
    : m_parseState(ParseJpegXLNotParsed)
    , m_quality(90)
    , m_currentimage_index(0)
    , m_previousimage_index(-1)
    , m_transformations(QImageIOHandler::TransformationNone)
    , m_decoder(nullptr)
    , m_runner(nullptr)
    , m_next_image_delay(0)
    , m_input_image_format(QImage::Format_Invalid)
    , m_target_image_format(QImage::Format_Invalid)
    , m_buffer_size(0)
{
}

QJpegXLHandler::~QJpegXLHandler()
{
    if (m_runner) {
        JxlThreadParallelRunnerDestroy(m_runner);
    }
    if (m_decoder) {
        JxlDecoderDestroy(m_decoder);
    }
}

bool QJpegXLHandler::canRead() const
{
    if (m_parseState == ParseJpegXLNotParsed && !canRead(device())) {
        return false;
    }

    if (m_parseState != ParseJpegXLError) {
        setFormat("jxl");

        if (m_parseState == ParseJpegXLFinished) {
            return false;
        }

        return true;
    }
    return false;
}

bool QJpegXLHandler::canRead(QIODevice *device)
{
    if (!device) {
        return false;
    }
    QByteArray header = device->peek(32);
    if (header.size() < 12) {
        return false;
    }

    JxlSignature signature = JxlSignatureCheck(reinterpret_cast<const uint8_t *>(header.constData()), header.size());
    if (signature == JXL_SIG_CODESTREAM || signature == JXL_SIG_CONTAINER) {
        return true;
    }
    return false;
}

bool QJpegXLHandler::ensureParsed() const
{
    if (m_parseState == ParseJpegXLSuccess || m_parseState == ParseJpegXLBasicInfoParsed || m_parseState == ParseJpegXLFinished) {
        return true;
    }
    if (m_parseState == ParseJpegXLError) {
        return false;
    }

    QJpegXLHandler *that = const_cast<QJpegXLHandler *>(this);

    return that->ensureDecoder();
}

bool QJpegXLHandler::ensureALLCounted() const
{
    if (!ensureParsed()) {
        return false;
    }

    if (m_parseState == ParseJpegXLSuccess || m_parseState == ParseJpegXLFinished) {
        return true;
    }

    QJpegXLHandler *that = const_cast<QJpegXLHandler *>(this);

    return that->countALLFrames();
}

bool QJpegXLHandler::ensureDecoder()
{
    if (m_decoder) {
        return true;
    }

    m_rawData = device()->readAll();

    if (m_rawData.isEmpty()) {
        return false;
    }

    JxlSignature signature = JxlSignatureCheck(reinterpret_cast<const uint8_t *>(m_rawData.constData()), m_rawData.size());
    if (signature != JXL_SIG_CODESTREAM && signature != JXL_SIG_CONTAINER) {
        m_parseState = ParseJpegXLError;
        return false;
    }

    m_decoder = JxlDecoderCreate(nullptr);
    if (!m_decoder) {
        qWarning("ERROR: JxlDecoderCreate failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

#ifdef JXL_QT_AUTOTRANSFORM
    // Let Qt handle the orientation.
    JxlDecoderSetKeepOrientation(m_decoder, true);
#endif

    int num_worker_threads = QThread::idealThreadCount();
    if (!m_runner && num_worker_threads >= 4) {
        /* use half of the threads because plug-in is usually used in environment
         * where application performs another tasks in backround (pre-load other images) */
        num_worker_threads = num_worker_threads / 2;
        num_worker_threads = qBound(2, num_worker_threads, 64);
        m_runner = JxlThreadParallelRunnerCreate(nullptr, num_worker_threads);

        if (JxlDecoderSetParallelRunner(m_decoder, JxlThreadParallelRunner, m_runner) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderSetParallelRunner failed");
            m_parseState = ParseJpegXLError;
            return false;
        }
    }

    if (JxlDecoderSetInput(m_decoder, reinterpret_cast<const uint8_t *>(m_rawData.constData()), m_rawData.size()) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetInput failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    JxlDecoderCloseInput(m_decoder);
#ifndef JXL_DECODE_BOXES_DISABLED
    JxlDecoderStatus status = JxlDecoderSubscribeEvents(m_decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME | JXL_DEC_BOX);
#else
    JxlDecoderStatus status = JxlDecoderSubscribeEvents(m_decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME);
#endif
    if (status == JXL_DEC_ERROR) {
        qWarning("ERROR: JxlDecoderSubscribeEvents failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    if (!decodeBoxes(status)) {
        return false;
    }

    status = JxlDecoderGetBasicInfo(m_decoder, &m_basicinfo);
    if (status != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JXL basic info not available");
        m_parseState = ParseJpegXLError;
        return false;
    }

    if (m_basicinfo.xsize == 0 || m_basicinfo.ysize == 0) {
        qWarning("ERROR: JXL image has zero dimensions");
        m_parseState = ParseJpegXLError;
        return false;
    }

    if (m_basicinfo.xsize > MAX_IMAGE_WIDTH || m_basicinfo.ysize > MAX_IMAGE_HEIGHT) {
        qWarning("JXL image (%dx%d) is too large", m_basicinfo.xsize, m_basicinfo.ysize);
        m_parseState = ParseJpegXLError;
        return false;
    }

    m_parseState = ParseJpegXLBasicInfoParsed;
    return true;
}

bool QJpegXLHandler::countALLFrames()
{
    if (m_parseState != ParseJpegXLBasicInfoParsed) {
        return false;
    }

    JxlDecoderStatus status;
    if (!decodeBoxes(status)) {
        return false;
    }

    if (status != JXL_DEC_COLOR_ENCODING) {
        qWarning("Unexpected event %d instead of JXL_DEC_COLOR_ENCODING", status);
        m_parseState = ParseJpegXLError;
        return false;
    }

    bool is_gray = m_basicinfo.num_color_channels == 1 && m_basicinfo.alpha_bits == 0;
    JxlColorEncoding color_encoding;
    if (m_basicinfo.uses_original_profile == JXL_FALSE && m_basicinfo.have_animation == JXL_FALSE) {
#if JPEGXL_NUMERIC_VERSION >= JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
        const JxlCmsInterface *jxlcms = JxlGetDefaultCms();
        if (jxlcms) {
            status = JxlDecoderSetCms(m_decoder, *jxlcms);
            if (status != JXL_DEC_SUCCESS) {
                qWarning("JxlDecoderSetCms ERROR");
            }
        } else {
            qWarning("No JPEG XL CMS Interface");
        }
#endif
        JxlColorEncodingSetToSRGB(&color_encoding, is_gray ? JXL_TRUE : JXL_FALSE);
        JxlDecoderSetPreferredColorProfile(m_decoder, &color_encoding);
    }

    bool loadalpha = false;
    if (m_basicinfo.alpha_bits > 0) {
        loadalpha = true;
    }

    m_input_pixel_format.endianness = JXL_NATIVE_ENDIAN;
    m_input_pixel_format.align = 4;
    m_input_pixel_format.num_channels = is_gray ? 1 : 4;

    if (m_basicinfo.bits_per_sample > 8) { // high bit depth
#ifdef JXL_HDR_PRESERVATION_DISABLED
        bool is_fp = false;
#else
        bool is_fp = m_basicinfo.exponent_bits_per_sample > 0 && m_basicinfo.num_color_channels == 3;
#endif

        if (is_gray) {
            m_input_pixel_format.data_type = JXL_TYPE_UINT16;
            m_input_image_format = m_target_image_format = QImage::Format_Grayscale16;
            m_buffer_size = ((size_t)m_basicinfo.ysize - 1) * (((((size_t)m_basicinfo.xsize) * 2 + 3) >> 2) << 2) + (size_t)m_basicinfo.xsize * 2;
        } else if (m_basicinfo.bits_per_sample > 16 && is_fp) {
            m_input_pixel_format.data_type = JXL_TYPE_FLOAT;
            m_input_image_format = QImage::Format_RGBA32FPx4;
            m_buffer_size = (size_t)m_basicinfo.xsize * (size_t)m_basicinfo.ysize * m_input_pixel_format.num_channels * 4;
            if (loadalpha)
                m_target_image_format = QImage::Format_RGBA32FPx4;
            else
                m_target_image_format = QImage::Format_RGBX32FPx4;
        } else {
            m_input_pixel_format.data_type = is_fp ? JXL_TYPE_FLOAT16 : JXL_TYPE_UINT16;
            m_buffer_size = (size_t)m_basicinfo.xsize * (size_t)m_basicinfo.ysize * m_input_pixel_format.num_channels * 2;
            m_input_image_format = is_fp ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA64;
            if (loadalpha)
                m_target_image_format = is_fp ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA64;
            else
                m_target_image_format = is_fp ? QImage::Format_RGBX16FPx4 : QImage::Format_RGBX64;
        }
    } else { // 8bit depth
        m_input_pixel_format.data_type = JXL_TYPE_UINT8;

        if (is_gray) {
            m_input_image_format = m_target_image_format = QImage::Format_Grayscale8;
            m_buffer_size = ((size_t)m_basicinfo.ysize - 1) * (((((size_t)m_basicinfo.xsize) + 3) >> 2) << 2) + (size_t)m_basicinfo.xsize;
        } else {
            m_input_image_format = QImage::Format_RGBA8888;
            m_buffer_size = (size_t)m_basicinfo.xsize * (size_t)m_basicinfo.ysize * m_input_pixel_format.num_channels;
            if (loadalpha) {
                m_target_image_format = QImage::Format_ARGB32;
            } else {
                m_target_image_format = QImage::Format_RGB32;
            }
        }
    }

    status = JxlDecoderGetColorAsEncodedProfile(m_decoder,
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
                                                &m_input_pixel_format,
#endif
                                                JXL_COLOR_PROFILE_TARGET_DATA,
                                                &color_encoding);

    if (status == JXL_DEC_SUCCESS && color_encoding.color_space == JXL_COLOR_SPACE_RGB && color_encoding.white_point == JXL_WHITE_POINT_D65
        && color_encoding.primaries == JXL_PRIMARIES_SRGB && color_encoding.transfer_function == JXL_TRANSFER_FUNCTION_SRGB) {
        m_colorspace = QColorSpace(QColorSpace::SRgb);
    } else {
        size_t icc_size = 0;
        if (JxlDecoderGetICCProfileSize(m_decoder,
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
                                        &m_input_pixel_format,
#endif
                                        JXL_COLOR_PROFILE_TARGET_DATA,
                                        &icc_size)
            == JXL_DEC_SUCCESS) {
            if (icc_size > 0) {
                QByteArray icc_data(icc_size, 0);
                if (JxlDecoderGetColorAsICCProfile(m_decoder,
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
                                                   &m_input_pixel_format,
#endif
                                                   JXL_COLOR_PROFILE_TARGET_DATA,
                                                   reinterpret_cast<uint8_t *>(icc_data.data()),
                                                   icc_data.size())
                    == JXL_DEC_SUCCESS) {
                    m_colorspace = QColorSpace::fromIccProfile(icc_data);

                    if (!m_colorspace.isValid()) {
                        qWarning("JXL image has Qt-unsupported or invalid ICC profile!");
                    }
                } else {
                    qWarning("Failed to obtain data from JPEG XL decoder");
                }
            } else {
                qWarning("Empty ICC data");
            }
        } else {
            qWarning("no ICC, other color profile");
        }
    }

    if (m_basicinfo.have_animation) { // count all frames
        JxlFrameHeader frame_header;
        int delay;

        for (status = JxlDecoderProcessInput(m_decoder); status != JXL_DEC_SUCCESS; status = JxlDecoderProcessInput(m_decoder)) {
            if (status != JXL_DEC_FRAME) {
                switch (status) {
                case JXL_DEC_ERROR:
                    qWarning("ERROR: JXL decoding failed");
                    break;
                case JXL_DEC_NEED_MORE_INPUT:
                    qWarning("ERROR: JXL data incomplete");
                    break;
                default:
                    qWarning("Unexpected event %d instead of JXL_DEC_FRAME", status);
                    break;
                }
                m_parseState = ParseJpegXLError;
                return false;
            }

            if (JxlDecoderGetFrameHeader(m_decoder, &frame_header) != JXL_DEC_SUCCESS) {
                qWarning("ERROR: JxlDecoderGetFrameHeader failed");
                m_parseState = ParseJpegXLError;
                return false;
            }

            if (m_basicinfo.animation.tps_denominator > 0 && m_basicinfo.animation.tps_numerator > 0) {
                delay = (int)(0.5 + 1000.0 * frame_header.duration * m_basicinfo.animation.tps_denominator / m_basicinfo.animation.tps_numerator);
            } else {
                delay = 0;
            }

            m_framedelays.append(delay);
        }

        if (m_framedelays.isEmpty()) {
            qWarning("no frames loaded by the JXL plug-in");
            m_parseState = ParseJpegXLError;
            return false;
        }

        if (m_framedelays.count() == 1) {
            qWarning("JXL file was marked as animation but it has only one frame.");
            m_basicinfo.have_animation = JXL_FALSE;
        }
    } else { // static picture
        m_framedelays.resize(1);
        m_framedelays[0] = 0;
    }

#ifndef JXL_DECODE_BOXES_DISABLED
    if (!decodeBoxes(status)) {
        return false;
    }
#endif

    if (!rewind()) {
        return false;
    }

    m_next_image_delay = m_framedelays[0];
    m_parseState = ParseJpegXLSuccess;
    return true;
}

bool QJpegXLHandler::decode_one_frame()
{
    JxlDecoderStatus status = JxlDecoderProcessInput(m_decoder);
    if (status != JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
        qWarning("Unexpected event %d instead of JXL_DEC_NEED_IMAGE_OUT_BUFFER", status);
        m_parseState = ParseJpegXLError;
        return false;
    }

    m_current_image = imageAlloc(m_basicinfo.xsize, m_basicinfo.ysize, m_input_image_format);
    if (m_current_image.isNull()) {
        qWarning("Memory cannot be allocated");
        m_parseState = ParseJpegXLError;
        return false;
    }

    m_current_image.setColorSpace(m_colorspace);
    if (!m_xmp.isEmpty()) {
        m_current_image.setText(QStringLiteral(META_KEY_XMP_ADOBE), QString::fromUtf8(m_xmp));
    }

    if (JxlDecoderSetImageOutBuffer(m_decoder, &m_input_pixel_format, m_current_image.bits(), m_buffer_size) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetImageOutBuffer failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    status = JxlDecoderProcessInput(m_decoder);
    if (status != JXL_DEC_FULL_IMAGE) {
        qWarning("Unexpected event %d instead of JXL_DEC_FULL_IMAGE", status);
        m_parseState = ParseJpegXLError;
        return false;
    }

    if (m_target_image_format != m_input_image_format) {
        m_current_image.convertTo(m_target_image_format);
    }

    m_next_image_delay = m_framedelays[m_currentimage_index];
    m_previousimage_index = m_currentimage_index;

    if (m_framedelays.count() > 1) {
        m_currentimage_index++;

        if (m_currentimage_index >= m_framedelays.count()) {
            if (!rewind()) {
                return false;
            }

            // all frames in animation have been read
            m_parseState = ParseJpegXLFinished;
        } else {
            m_parseState = ParseJpegXLSuccess;
        }
    } else {
        // the static image has been read
        m_parseState = ParseJpegXLFinished;
    }

    return true;
}

bool QJpegXLHandler::read(QImage *image)
{
    if (!ensureALLCounted()) {
        return false;
    }

    if (m_currentimage_index == m_previousimage_index) {
        *image = m_current_image;
        return jumpToNextImage();
    }

    if (decode_one_frame()) {
        *image = m_current_image;
        return true;
    } else {
        return false;
    }
}

template<class T>
void packRGBPixels(QImage &img)
{
    // pack pixel data
    auto dest_pixels = reinterpret_cast<T *>(img.bits());
    for (qint32 y = 0; y < img.height(); y++) {
        auto src_pixels = reinterpret_cast<const T *>(img.constScanLine(y));
        for (qint32 x = 0; x < img.width(); x++) {
            // R
            *dest_pixels = *src_pixels;
            dest_pixels++;
            src_pixels++;
            // G
            *dest_pixels = *src_pixels;
            dest_pixels++;
            src_pixels++;
            // B
            *dest_pixels = *src_pixels;
            dest_pixels++;
            src_pixels += 2; // skipalpha
        }
    }
}

bool QJpegXLHandler::write(const QImage &image)
{
    if (image.format() == QImage::Format_Invalid) {
        qWarning("No image data to save");
        return false;
    }

    if ((image.width() == 0) || (image.height() == 0)) {
        qWarning("Image has zero dimension!");
        return false;
    }

    if ((image.width() > MAX_IMAGE_WIDTH) || (image.height() > MAX_IMAGE_HEIGHT)) {
        qWarning("Image (%dx%d) is too large to save!", image.width(), image.height());
        return false;
    }

    size_t pixel_count = size_t(image.width()) * image.height();
    if (MAX_IMAGE_PIXELS && pixel_count > MAX_IMAGE_PIXELS) {
        qWarning("Image (%dx%d) will not be saved because it has more than %d megapixels!", image.width(), image.height(), MAX_IMAGE_PIXELS / 1024 / 1024);
        return false;
    }

    int save_depth = 8; // 8 / 16 / 32
    bool save_fp = false;
    bool is_gray = false;
    // depth detection
    switch (image.format()) {
    case QImage::Format_RGBX32FPx4:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
#ifndef JXL_HDR_PRESERVATION_DISABLED
        save_depth = 32;
        save_fp = true;
        break;
#endif
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
#ifndef JXL_HDR_PRESERVATION_DISABLED
        save_depth = 16;
        save_fp = true;
        break;
#endif
    case QImage::Format_BGR30:
    case QImage::Format_A2BGR30_Premultiplied:
    case QImage::Format_RGB30:
    case QImage::Format_A2RGB30_Premultiplied:
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
        save_depth = 16;
        break;
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGB888:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    case QImage::Format_CMYK8888:
#endif
        save_depth = 8;
        break;
    case QImage::Format_Grayscale16:
        save_depth = 16;
        is_gray = true;
        break;
    case QImage::Format_Grayscale8:
    case QImage::Format_Alpha8:
    case QImage::Format_Mono:
    case QImage::Format_MonoLSB:
        save_depth = 8;
        is_gray = true;
        break;
    case QImage::Format_Indexed8:
        save_depth = 8;
        is_gray = image.isGrayscale();
        break;
    default:
        if (image.depth() > 32) {
            save_depth = 16;
        } else {
            save_depth = 8;
        }
        break;
    }

    JxlEncoder *encoder = JxlEncoderCreate(nullptr);
    if (!encoder) {
        qWarning("Failed to create Jxl encoder");
        return false;
    }
    JxlEncoderUseBoxes(encoder);

    if (m_quality > 100) {
        m_quality = 100;
    } else if (m_quality < 0) {
        m_quality = 90;
    }

    JxlBasicInfo output_info;
    JxlEncoderInitBasicInfo(&output_info);

    QByteArray iccprofile;
    QColorSpace tmpcs = image.colorSpace();
    if (!tmpcs.isValid() || tmpcs.primaries() != QColorSpace::Primaries::SRgb || tmpcs.transferFunction() != QColorSpace::TransferFunction::SRgb
        || m_quality == 100) {
        // no profile or Qt-unsupported ICC profile
        iccprofile = tmpcs.iccProfile();
        // note: lossless encoding requires uses_original_profile = JXL_TRUE
        if (iccprofile.size() > 0 || m_quality == 100 || is_gray) {
            output_info.uses_original_profile = JXL_TRUE;
        }
    }

    // clang-format off
    if (   (save_depth > 8 && (image.hasAlphaChannel() || output_info.uses_original_profile))
        || (save_depth > 16)
        || (pixel_count > FEATURE_LEVEL_5_PIXELS)
        || (image.width() > FEATURE_LEVEL_5_WIDTH)
        || (image.height() > FEATURE_LEVEL_5_HEIGHT)) {
        output_info.have_container = JXL_TRUE;
        JxlEncoderUseContainer(encoder, JXL_TRUE);
        JxlEncoderSetCodestreamLevel(encoder, 10);
    }
    // clang-format on

    void *runner = nullptr;
    int num_worker_threads = qBound(1, QThread::idealThreadCount(), 64);

    if (num_worker_threads > 1) {
        runner = JxlThreadParallelRunnerCreate(nullptr, num_worker_threads);
        if (JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner, runner) != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetParallelRunner failed");
            JxlThreadParallelRunnerDestroy(runner);
            JxlEncoderDestroy(encoder);
            return false;
        }
    }

    JxlPixelFormat pixel_format;
    QImage::Format tmpformat;
    JxlEncoderStatus status;

    pixel_format.endianness = JXL_NATIVE_ENDIAN;
    pixel_format.align = 0;

    output_info.animation.tps_numerator = 10;
    output_info.animation.tps_denominator = 1;
    output_info.orientation = JXL_ORIENT_IDENTITY;
    if (m_transformations == QImageIOHandler::TransformationMirror) {
        output_info.orientation = JXL_ORIENT_FLIP_HORIZONTAL;
    } else if (m_transformations == QImageIOHandler::TransformationRotate180) {
        output_info.orientation = JXL_ORIENT_ROTATE_180;
    } else if (m_transformations == QImageIOHandler::TransformationFlip) {
        output_info.orientation = JXL_ORIENT_FLIP_VERTICAL;
    } else if (m_transformations == QImageIOHandler::TransformationFlipAndRotate90) {
        output_info.orientation = JXL_ORIENT_TRANSPOSE;
    } else if (m_transformations == QImageIOHandler::TransformationRotate90) {
        output_info.orientation = JXL_ORIENT_ROTATE_90_CW;
    } else if (m_transformations == QImageIOHandler::TransformationMirrorAndRotate90) {
        output_info.orientation = JXL_ORIENT_ANTI_TRANSPOSE;
    } else if (m_transformations == QImageIOHandler::TransformationRotate270) {
        output_info.orientation = JXL_ORIENT_ROTATE_90_CCW;
    }

    if (save_depth > 8 && is_gray) { // 16bit depth gray
        pixel_format.data_type = JXL_TYPE_UINT16;
        pixel_format.align = 4;
        output_info.num_color_channels = 1;
        output_info.bits_per_sample = 16;
        tmpformat = QImage::Format_Grayscale16;
        pixel_format.num_channels = 1;
    } else if (is_gray) { // 8bit depth gray
        pixel_format.data_type = JXL_TYPE_UINT8;
        pixel_format.align = 4;
        output_info.num_color_channels = 1;
        output_info.bits_per_sample = 8;
        tmpformat = QImage::Format_Grayscale8;
        pixel_format.num_channels = 1;
    } else if (save_depth > 16) { // 32bit depth rgb
        pixel_format.data_type = JXL_TYPE_FLOAT;
        output_info.exponent_bits_per_sample = 8;
        output_info.num_color_channels = 3;
        output_info.bits_per_sample = 32;

        if (image.hasAlphaChannel()) {
            tmpformat = QImage::Format_RGBA32FPx4;
            pixel_format.num_channels = 4;
            output_info.alpha_bits = 32;
            output_info.alpha_exponent_bits = 8;
            output_info.num_extra_channels = 1;
        } else {
            tmpformat = QImage::Format_RGBX32FPx4;
            pixel_format.num_channels = 3;
            output_info.alpha_bits = 0;
            output_info.num_extra_channels = 0;
        }
    } else if (save_depth > 8) { // 16bit depth rgb
        pixel_format.data_type = save_fp ? JXL_TYPE_FLOAT16 : JXL_TYPE_UINT16;
        output_info.exponent_bits_per_sample = save_fp ? 5 : 0;
        output_info.num_color_channels = 3;
        output_info.bits_per_sample = 16;

        if (image.hasAlphaChannel()) {
            tmpformat = save_fp ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA64;
            pixel_format.num_channels = 4;
            output_info.alpha_bits = 16;
            output_info.alpha_exponent_bits = save_fp ? 5 : 0;
            output_info.num_extra_channels = 1;
        } else {
            tmpformat = save_fp ? QImage::Format_RGBX16FPx4 : QImage::Format_RGBX64;
            pixel_format.num_channels = 3;
            output_info.alpha_bits = 0;
            output_info.num_extra_channels = 0;
        }
    } else { // 8bit depth rgb
        pixel_format.data_type = JXL_TYPE_UINT8;
        pixel_format.align = 4;
        output_info.num_color_channels = 3;
        output_info.bits_per_sample = 8;

        if (image.hasAlphaChannel()) {
            tmpformat = QImage::Format_RGBA8888;
            pixel_format.num_channels = 4;
            output_info.alpha_bits = 8;
            output_info.num_extra_channels = 1;
        } else {
            tmpformat = QImage::Format_RGB888;
            pixel_format.num_channels = 3;
            output_info.alpha_bits = 0;
            output_info.num_extra_channels = 0;
        }
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // TODO: add native CMYK support (libjxl supports CMYK images)
    QImage tmpimage;
    auto cs = image.colorSpace();
    if (cs.isValid() && cs.colorModel() == QColorSpace::ColorModel::Cmyk && image.format() == QImage::Format_CMYK8888) {
        tmpimage = image.convertedToColorSpace(QColorSpace(QColorSpace::SRgb), tmpformat);
    } else {
        tmpimage = image.convertToFormat(tmpformat);
    }
#else
    QImage tmpimage = image.convertToFormat(tmpformat);
#endif

    const size_t xsize = tmpimage.width();
    const size_t ysize = tmpimage.height();

    if (xsize == 0 || ysize == 0 || tmpimage.isNull()) {
        qWarning("Unable to allocate memory for output image");
        if (runner) {
            JxlThreadParallelRunnerDestroy(runner);
        }
        JxlEncoderDestroy(encoder);
        return false;
    }

    output_info.xsize = tmpimage.width();
    output_info.ysize = tmpimage.height();

    status = JxlEncoderSetBasicInfo(encoder, &output_info);
    if (status != JXL_ENC_SUCCESS) {
        qWarning("JxlEncoderSetBasicInfo failed!");
        if (runner) {
            JxlThreadParallelRunnerDestroy(runner);
        }
        JxlEncoderDestroy(encoder);
        return false;
    }

    auto xmp_data = image.text(QStringLiteral(META_KEY_XMP_ADOBE)).toUtf8();
    if (!xmp_data.isEmpty()) {
        const char *box_type = "xml ";
        status = JxlEncoderAddBox(encoder, box_type, reinterpret_cast<const uint8_t *>(xmp_data.constData()), xmp_data.size(), JXL_FALSE);
        if (status != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderAddBox failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }
    }
    JxlEncoderCloseBoxes(encoder); // no more metadata

    if (iccprofile.size() > 0) {
        status = JxlEncoderSetICCProfile(encoder, reinterpret_cast<const uint8_t *>(iccprofile.constData()), iccprofile.size());
        if (status != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetICCProfile failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }
    } else {
        JxlColorEncoding color_profile;
        JxlColorEncodingSetToSRGB(&color_profile, is_gray ? JXL_TRUE : JXL_FALSE);

        status = JxlEncoderSetColorEncoding(encoder, &color_profile);
        if (status != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetColorEncoding failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }
    }

    JxlEncoderFrameSettings *encoder_options = JxlEncoderFrameSettingsCreate(encoder, nullptr);

    JxlEncoderSetFrameDistance(encoder_options, (100.0f - m_quality) / 10.0f);

    JxlEncoderSetFrameLossless(encoder_options, (m_quality == 100) ? JXL_TRUE : JXL_FALSE);

    size_t buffer_size = size_t(tmpimage.bytesPerLine()) * tmpimage.height();
    if (!image.hasAlphaChannel() && save_depth > 8 && !is_gray) { // pack pixel on tmpimage
        buffer_size = (size_t(save_depth / 8) * pixel_format.num_channels * xsize * ysize);

        // detaching image
        tmpimage.detach();
        if (tmpimage.isNull()) {
            qWarning("Memory allocation error");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        // pack pixel data
        if (save_depth > 16 && save_fp)
            packRGBPixels<float>(tmpimage);
        else if (save_fp)
            packRGBPixels<qfloat16>(tmpimage);
        else
            packRGBPixels<quint16>(tmpimage);
    }
    status = JxlEncoderAddImageFrame(encoder_options, &pixel_format, static_cast<const void *>(tmpimage.constBits()), buffer_size);

    if (status == JXL_ENC_ERROR) {
        qWarning("JxlEncoderAddImageFrame failed!");
        if (runner) {
            JxlThreadParallelRunnerDestroy(runner);
        }
        JxlEncoderDestroy(encoder);
        return false;
    }

    JxlEncoderCloseInput(encoder);

    std::vector<uint8_t> compressed;
    compressed.resize(4096);
    size_t offset = 0;
    uint8_t *next_out;
    size_t avail_out;
    do {
        next_out = compressed.data() + offset;
        avail_out = compressed.size() - offset;
        status = JxlEncoderProcessOutput(encoder, &next_out, &avail_out);

        if (status == JXL_ENC_NEED_MORE_OUTPUT) {
            offset = next_out - compressed.data();
            compressed.resize(compressed.size() * 2);
        } else if (status == JXL_ENC_ERROR) {
            qWarning("JxlEncoderProcessOutput failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }
    } while (status != JXL_ENC_SUCCESS);

    if (runner) {
        JxlThreadParallelRunnerDestroy(runner);
    }
    JxlEncoderDestroy(encoder);

    compressed.resize(next_out - compressed.data());

    if (compressed.size() > 0) {
        qint64 write_status = device()->write(reinterpret_cast<const char *>(compressed.data()), compressed.size());

        if (write_status > 0) {
            return true;
        } else if (write_status == -1) {
            qWarning("Write error: %s\n", qUtf8Printable(device()->errorString()));
        }
    }

    return false;
}

QVariant QJpegXLHandler::option(ImageOption option) const
{
    if (!supportsOption(option)) {
        return QVariant();
    }

    if (option == Quality) {
        return m_quality;
    }

    if (!ensureParsed()) {
#ifdef JXL_QT_AUTOTRANSFORM
        if (option == ImageTransformation) {
            return int(m_transformations);
        }
#endif
        return QVariant();
    }

    switch (option) {
    case Size:
        return QSize(m_basicinfo.xsize, m_basicinfo.ysize);
    case Animation:
        if (m_basicinfo.have_animation) {
            return true;
        } else {
            return false;
        }
#ifdef JXL_QT_AUTOTRANSFORM
    case ImageTransformation:
        if (m_basicinfo.orientation == JXL_ORIENT_IDENTITY) {
            return int(QImageIOHandler::TransformationNone);
        } else if (m_basicinfo.orientation == JXL_ORIENT_FLIP_HORIZONTAL) {
            return int(QImageIOHandler::TransformationMirror);
        } else if (m_basicinfo.orientation == JXL_ORIENT_ROTATE_180) {
            return int(QImageIOHandler::TransformationRotate180);
        } else if (m_basicinfo.orientation == JXL_ORIENT_FLIP_VERTICAL) {
            return int(QImageIOHandler::TransformationFlip);
        } else if (m_basicinfo.orientation == JXL_ORIENT_TRANSPOSE) {
            return int(QImageIOHandler::TransformationFlipAndRotate90);
        } else if (m_basicinfo.orientation == JXL_ORIENT_ROTATE_90_CW) {
            return int(QImageIOHandler::TransformationRotate90);
        } else if (m_basicinfo.orientation == JXL_ORIENT_ANTI_TRANSPOSE) {
            return int(QImageIOHandler::TransformationMirrorAndRotate90);
        } else if (m_basicinfo.orientation == JXL_ORIENT_ROTATE_90_CCW) {
            return int(QImageIOHandler::TransformationRotate270);
        }
        break;
#endif
    default:
        return QVariant();
    }

    return QVariant();
}

void QJpegXLHandler::setOption(ImageOption option, const QVariant &value)
{
    switch (option) {
    case Quality:
        m_quality = value.toInt();
        if (m_quality > 100) {
            m_quality = 100;
        } else if (m_quality < 0) {
            m_quality = 90;
        }
        return;
#ifdef JXL_QT_AUTOTRANSFORM
    case ImageTransformation:
        if (auto t = value.toInt()) {
            if (t > 0 && t < 8)
                m_transformations = QImageIOHandler::Transformations(t);
        }
        break;
#endif
    default:
        break;
    }
    QImageIOHandler::setOption(option, value);
}

bool QJpegXLHandler::supportsOption(ImageOption option) const
{
    auto supported = option == Quality || option == Size || option == Animation;
#ifdef JXL_QT_AUTOTRANSFORM
    supported = supported || option == ImageTransformation;
#endif
    return supported;
}

int QJpegXLHandler::imageCount() const
{
    if (!ensureParsed()) {
        return 0;
    }

    if (m_parseState == ParseJpegXLBasicInfoParsed) {
        if (!m_basicinfo.have_animation) {
            return 1;
        }

        if (!ensureALLCounted()) {
            return 0;
        }
    }

    if (!m_framedelays.isEmpty()) {
        return m_framedelays.count();
    }
    return 0;
}

int QJpegXLHandler::currentImageNumber() const
{
    if (m_parseState == ParseJpegXLNotParsed) {
        return -1;
    }

    if (m_parseState == ParseJpegXLError || m_parseState == ParseJpegXLBasicInfoParsed || !m_decoder) {
        return 0;
    }

    return m_currentimage_index;
}

bool QJpegXLHandler::jumpToNextImage()
{
    if (!ensureALLCounted()) {
        return false;
    }

    if (m_framedelays.count() > 1) {
        m_currentimage_index++;

        if (m_currentimage_index >= m_framedelays.count()) {
            if (!rewind()) {
                return false;
            }
        } else {
            JxlDecoderSkipFrames(m_decoder, 1);
        }
    }

    m_parseState = ParseJpegXLSuccess;
    return true;
}

bool QJpegXLHandler::jumpToImage(int imageNumber)
{
    if (!ensureALLCounted()) {
        return false;
    }

    if (imageNumber < 0 || imageNumber >= m_framedelays.count()) {
        return false;
    }

    if (imageNumber == m_currentimage_index) {
        m_parseState = ParseJpegXLSuccess;
        return true;
    }

    if (imageNumber > m_currentimage_index) {
        JxlDecoderSkipFrames(m_decoder, imageNumber - m_currentimage_index);
        m_currentimage_index = imageNumber;
        m_parseState = ParseJpegXLSuccess;
        return true;
    }

    if (!rewind()) {
        return false;
    }

    if (imageNumber > 0) {
        JxlDecoderSkipFrames(m_decoder, imageNumber);
    }
    m_currentimage_index = imageNumber;
    m_parseState = ParseJpegXLSuccess;
    return true;
}

int QJpegXLHandler::nextImageDelay() const
{
    if (!ensureALLCounted()) {
        return 0;
    }

    if (m_framedelays.count() < 2) {
        return 0;
    }

    return m_next_image_delay;
}

int QJpegXLHandler::loopCount() const
{
    if (!ensureParsed()) {
        return 0;
    }

    if (m_basicinfo.have_animation) {
        return (m_basicinfo.animation.num_loops > 0) ? m_basicinfo.animation.num_loops - 1 : -1;
    } else {
        return 0;
    }
}

bool QJpegXLHandler::rewind()
{
    m_currentimage_index = 0;

    JxlDecoderReleaseInput(m_decoder);
    JxlDecoderRewind(m_decoder);
    if (m_runner) {
        if (JxlDecoderSetParallelRunner(m_decoder, JxlThreadParallelRunner, m_runner) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderSetParallelRunner failed");
            m_parseState = ParseJpegXLError;
            return false;
        }
    }

    if (JxlDecoderSetInput(m_decoder, reinterpret_cast<const uint8_t *>(m_rawData.constData()), m_rawData.size()) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetInput failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    JxlDecoderCloseInput(m_decoder);

    if (m_basicinfo.uses_original_profile == JXL_FALSE && m_basicinfo.have_animation == JXL_FALSE) {
        if (JxlDecoderSubscribeEvents(m_decoder, JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderSubscribeEvents failed");
            m_parseState = ParseJpegXLError;
            return false;
        }

        JxlDecoderStatus status = JxlDecoderProcessInput(m_decoder);
        if (status != JXL_DEC_COLOR_ENCODING) {
            qWarning("Unexpected event %d instead of JXL_DEC_COLOR_ENCODING", status);
            m_parseState = ParseJpegXLError;
            return false;
        }

#if JPEGXL_NUMERIC_VERSION >= JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
        const JxlCmsInterface *jxlcms = JxlGetDefaultCms();
        if (jxlcms) {
            status = JxlDecoderSetCms(m_decoder, *jxlcms);
            if (status != JXL_DEC_SUCCESS) {
                qWarning("JxlDecoderSetCms ERROR");
            }
        } else {
            qWarning("No JPEG XL CMS Interface");
        }
#endif

        bool is_gray = m_basicinfo.num_color_channels == 1 && m_basicinfo.alpha_bits == 0;
        JxlColorEncoding color_encoding;
        JxlColorEncodingSetToSRGB(&color_encoding, is_gray ? JXL_TRUE : JXL_FALSE);
        JxlDecoderSetPreferredColorProfile(m_decoder, &color_encoding);
    } else {
        if (JxlDecoderSubscribeEvents(m_decoder, JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderSubscribeEvents failed");
            m_parseState = ParseJpegXLError;
            return false;
        }
    }

    return true;
}

bool QJpegXLHandler::decodeBoxes(JxlDecoderStatus &status)
{
    do { // decode metadata
        status = JxlDecoderProcessInput(m_decoder);
        if (status == JXL_DEC_BOX) {
            JxlBoxType type;
            JxlDecoderGetBoxType(m_decoder, type, JXL_FALSE);
            if (memcmp(type, "xml ", 4) == 0) {
                uint64_t size;
                if (JxlDecoderGetBoxSizeRaw(m_decoder, &size) == JXL_DEC_SUCCESS && size < uint64_t(kMaxQVectorSize)) {
                    m_xmp = QByteArray(size, '\0');
                    JxlDecoderSetBoxBuffer(m_decoder, reinterpret_cast<uint8_t *>(m_xmp.data()), m_xmp.size());
                }
            }
        }
    } while (status == JXL_DEC_BOX);

    if (status == JXL_DEC_ERROR) {
        qWarning("ERROR: JXL decoding failed");
        m_parseState = ParseJpegXLError;
        return false;
    }
    if (status == JXL_DEC_NEED_MORE_INPUT) {
        qWarning("ERROR: JXL data incomplete");
        m_parseState = ParseJpegXLError;
        return false;
    }
    return true;
}

QImageIOPlugin::Capabilities QJpegXLPlugin::capabilities(QIODevice *device, const QByteArray &format) const
{
    if (format == "jxl") {
        return Capabilities(CanRead | CanWrite);
    }

    if (!format.isEmpty()) {
        return {};
    }
    if (!device->isOpen()) {
        return {};
    }

    Capabilities cap;
    if (device->isReadable() && QJpegXLHandler::canRead(device)) {
        cap |= CanRead;
    }

    if (device->isWritable()) {
        cap |= CanWrite;
    }

    return cap;
}

QImageIOHandler *QJpegXLPlugin::create(QIODevice *device, const QByteArray &format) const
{
    QImageIOHandler *handler = new QJpegXLHandler;
    handler->setDevice(device);
    handler->setFormat(format);
    return handler;
}

#include "moc_jxl_p.cpp"
