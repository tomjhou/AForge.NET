// AForge FFMPEG Library
// AForge.NET framework
// http://www.aforgenet.com/framework/
//
// Copyright © AForge.NET, 2009-2012
// contacts@aforgenet.com
//

#include "StdAfx.h"
#include "VideoFileReader.h"

namespace libffmpeg
{
	extern "C"
	{
		// disable warnings about badly formed documentation from FFmpeg, which we don't need at all
		#pragma warning(disable:4635) 
		// disable warning about conversion int64 to int32
		#pragma warning(disable:4244) 

		#include "libavformat\avformat.h"
		#include "libavformat\avio.h"
		#include "libavcodec\avcodec.h"
		#include "libswscale\swscale.h"
	}
}

namespace AForge { namespace Video { namespace FFMPEG
{
#pragma region Some private FFmpeg related stuff hidden out of header file

// A structure to encapsulate all FFMPEG related private variable
ref struct ReaderPrivateData
{
public:
	libffmpeg::AVFormatContext*		FormatContext;
	libffmpeg::AVStream*			VideoStream;
	libffmpeg::AVCodecContext*		CodecContext;
	libffmpeg::AVFrame*				VideoFrame;
	struct libffmpeg::SwsContext*	ConvertContext;

	libffmpeg::AVPacket* Packet;
	int BytesRemaining;
	Int64 frame_number;

	ReaderPrivateData( )
	{
		FormatContext     = NULL;
		VideoStream       = NULL;
		CodecContext      = NULL;
		VideoFrame        = NULL;
		ConvertContext	  = NULL;

		Packet  = NULL;
		BytesRemaining = 0;
	}
};
#pragma endregion

// Class constructor
VideoFileReader::VideoFileReader( void ) :
    data( nullptr ), disposed( false )
{	
	libffmpeg::av_register_all( );
}

#pragma managed(push, off)
static libffmpeg::AVFormatContext* open_file( char* fileName )
{
	libffmpeg::AVFormatContext* formatContext;

	if ( libffmpeg::av_open_input_file( &formatContext, fileName, NULL, 0, NULL ) !=0 )
	{
		return NULL;
	}
	return formatContext;
}
#pragma managed(pop)

// Opens the specified video file
void VideoFileReader::Open( String^ fileName )
{
    CheckIfDisposed( );

	// close previous file if any was open
	Close( );

	data = gcnew ReaderPrivateData( );
	data->Packet = new libffmpeg::AVPacket( );
	data->Packet->data = NULL;

	bool success = false;

	// convert specified managed String to UTF8 unmanaged string
	IntPtr ptr = System::Runtime::InteropServices::Marshal::StringToHGlobalUni( fileName );
    wchar_t* nativeFileNameUnicode = (wchar_t*) ptr.ToPointer( );
    int utf8StringSize = WideCharToMultiByte( CP_UTF8, 0, nativeFileNameUnicode, -1, NULL, 0, NULL, NULL );
    char* nativeFileName = new char[utf8StringSize];
    WideCharToMultiByte( CP_UTF8, 0, nativeFileNameUnicode, -1, nativeFileName, utf8StringSize, NULL, NULL );

	try
	{
		// open the specified video file
		data->FormatContext = open_file( nativeFileName );
		if ( data->FormatContext == NULL )
			throw gcnew System::IO::IOException( "Cannot open the video file." );

		// retrieve stream information
		if ( libffmpeg::av_find_stream_info( data->FormatContext ) < 0 )
			throw gcnew VideoException( "Cannot find stream information." );

		// DEBUGGING
#if _DEBUG
		int ns = data->FormatContext->nb_streams;
		Int64 firstData = data->FormatContext->start_time;
		Int64 dur = data->FormatContext->duration;
#endif
		// search for the first video stream
		for ( unsigned int i = 0; i < data->FormatContext->nb_streams; i++ )
		{
			if( data->FormatContext->streams[i]->codec->codec_type == libffmpeg::AVMEDIA_TYPE_VIDEO )
			{
				// get the pointer to the codec context for the video stream
				data->CodecContext = data->FormatContext->streams[i]->codec;
				data->VideoStream  = data->FormatContext->streams[i];
				break;
			}
		}
		if ( data->VideoStream == NULL )
			throw gcnew VideoException( "Cannot find video stream in the specified file." );

		// DEBUG INSPECTION
#if _DEBUG
		libffmpeg::AVStream* avstr = data->VideoStream;
		Int64 fdts = avstr->first_dts;
		libffmpeg::AVRational a = avstr->avg_frame_rate;
		int num = a.num;
		int den = a.den;
#endif
		// find decoder for the video stream
		libffmpeg::AVCodec* codec = libffmpeg::avcodec_find_decoder( data->CodecContext->codec_id );
		if ( codec == NULL )
			throw gcnew VideoException( "Cannot find codec to decode the video stream." );

		// open the codec
		if ( libffmpeg::avcodec_open( data->CodecContext, codec ) < 0 )
			throw gcnew VideoException( "Cannot open video codec." );

		// allocate video frame
		data->VideoFrame = libffmpeg::avcodec_alloc_frame( );

		// prepare scaling context to convert RGB image to video format
		data->ConvertContext = libffmpeg::sws_getContext( data->CodecContext->width, data->CodecContext->height, // Source width (from video file)
				data->CodecContext->pix_fmt,
				data->CodecContext->width, data->CodecContext->height,	// Destination width (same as source width)
				libffmpeg::PIX_FMT_BGR24,
				SWS_BICUBIC, NULL, NULL, NULL );

		if ( data->ConvertContext == NULL )
			throw gcnew VideoException( "Cannot initialize frames conversion context." );

		// get some properties of the video file
		m_width  = data->CodecContext->width;
		m_height = data->CodecContext->height;

		data->frame_number = 0;

		// DEBUGGING
#if _DEBUG
		num = data->VideoStream->r_frame_rate.num;
		den = data->VideoStream->r_frame_rate.den;
		libffmpeg::AVRational stream_time_base = data->VideoStream->time_base;
		int num2 = stream_time_base.num;
		int den2 = stream_time_base.den;
#endif
		m_frameRate = data->VideoStream->r_frame_rate.num / data->VideoStream->r_frame_rate.den;
		m_codecName = gcnew String( data->CodecContext->codec->name );
		m_framesCount = data->VideoStream->nb_frames;

		success = true;
	}
	finally
	{
		System::Runtime::InteropServices::Marshal::FreeHGlobal( ptr );
        delete [] nativeFileName;

		if ( !success )
			Close( );
	}
}

// Close current video file
void VideoFileReader::Close(  )
{
	if ( data != nullptr )
	{
		if ( data->VideoFrame != NULL )
			libffmpeg::av_free( data->VideoFrame );

		if ( data->CodecContext != NULL )
			libffmpeg::avcodec_close( data->CodecContext );

		if ( data->FormatContext != NULL )
			libffmpeg::av_close_input_file( data->FormatContext );

		if ( data->ConvertContext != NULL )
			libffmpeg::sws_freeContext( data->ConvertContext );

		if ( data->Packet->data != NULL )
			libffmpeg::av_free_packet( data->Packet );

		data = nullptr;
	}
}

#if _DEBUG
Int64 frameNumPts;
int duration;
bool weirdFlag = false;
#endif

// Becomes true once frame is read and decoded into memory. After it is "gotten", i.e.
// copied to Bitmap and returned to caller, this flag becomes false.
bool haveDecodedFrameToFetch = false;

// Read next video frame of the current video file.
// If a frame is already in memory (from previous ReadVideoFrameBasic) but not yet decoded, this 
// will decode that frame to memory, and return. Frame is NOT converted to Bitmap, nor returned to user
// in any other way, but next call to FetchVideoFrame() will retrieve it.
//
// If no frame is currently in memory, or , will read next frame, then decode and return.
//
// If frame seems incomplete (i.e. decoder does not set &frameFinished flag),
// will continue to read frames until decoder is happy.
bool VideoFileReader::ReadVideoFrameBasic(  )
{
    CheckIfDisposed( );

	if ( data == nullptr )
		throw gcnew System::IO::IOException( "Cannot read video frames since video file is not open." );

	int frameFinished;

	int bytesDecoded = 0;
	bool exit = false;

	while ( true )
	{
		// work on the current packet until we have decoded all of it
		while ( data->BytesRemaining > 0 )
		{
			// decode the next chunk of data
			bytesDecoded = libffmpeg::avcodec_decode_video2( data->CodecContext, data->VideoFrame, &frameFinished, data->Packet );

			// was there an error?
			if ( bytesDecoded < 0 )
				throw gcnew VideoException( "Error while decoding frame." );

			data->BytesRemaining -= bytesDecoded;
					 
			// did we finish the current frame? Then we can return
			if ( frameFinished )
			{
				data->frame_number = data->Packet->dts;
				haveDecodedFrameToFetch = true;
				return true;
			}
		}

		// If we are here, then one of two things happened:
		// (1) There was no previous undecoded frame in memory.
		// (2) The previous undecoded frame got decoded (possibly in multiple passes) but &frameFinished flag
		//     was not set. This seems to happen sometimes at the very beginning of a file. Incomplete/missing frames?

#if _DEBUG
		// DEBUG INSPECTION
		frameNumPts = data->Packet->pts;
		duration = data->Packet->duration;
		if (bytesDecoded > 0)
		{
			// Incomplete/missing frame? Why does this happen?
			weirdFlag = true;
		}
#endif

		// read the next packet, skipping all packets that aren't
		// for this stream
		do
		{
			// free old packet if any
			if ( data->Packet->data != NULL )
			{
				libffmpeg::av_free_packet( data->Packet );
				data->Packet->data = NULL;
			}

			// read new packet
			if ( libffmpeg::av_read_frame( data->FormatContext, data->Packet ) < 0)
			{
				// Error or EOF
				exit = true;
				break;
			}
		}
		while ( data->Packet->stream_index != data->VideoStream->index );

		// exit ?
		if ( exit )
			break;

		// Set up for decoding ...
		data->BytesRemaining = data->Packet->size;

		// DEBUG INSPECTION
#if _DEBUG
		duration = data->Packet->duration;
#endif
	}

	// If we are here, it is because loop was broken from error, or EOF

	// decode the rest of the last frame
	bytesDecoded = libffmpeg::avcodec_decode_video2(
		data->CodecContext, data->VideoFrame, &frameFinished, data->Packet );

	// free last packet
	if ( data->Packet->data != NULL )
	{
		libffmpeg::av_free_packet( data->Packet );
		data->Packet->data = NULL;
	}

	haveDecodedFrameToFetch = frameFinished;

	return frameFinished;
}

// Seeks until specified frame is reached and decoded. Does not actually return the
// frame as a bitmap, but next ReadVideoFrame() will do that.
//
// Returns true if successful, false if not.
bool VideoFileReader::SeekFrame(Int64 frame)
{
	int frame_delta = frame - data->frame_number;
	int success = 0;

	if (frame_delta < 0 || frame_delta > 5)
	{
		// Seeks to nearest key frame BEFORE the specified frame. We only do this if
		// we need to go forward > 5 frames, or go backwards.
		success = libffmpeg::av_seek_frame(data->FormatContext, data->VideoStream->index,
			frame, AVSEEK_FLAG_BACKWARD);

		if (success < 0)
			return false;
	}

	if (data->frame_number == frame)
	{
		if (haveDecodedFrameToFetch)
			// If already at exactly the right frame, and it is not yet fetched, just do nothing
			return true;
		else
		{
			// We are at the right frame, but it is already decoded? Or was never read/decoded in the first place? Just try to read it again.
			haveDecodedFrameToFetch = ReadVideoFrameBasic();
			if (!haveDecodedFrameToFetch)
				// something failed
				return false;
			if (data->frame_number == frame)
				// This worked. Most likely, frame was never read/decoded in the first place, and now it is.
				return true;

			if (data->frame_number > frame)
				// We got the NEXT frame. Most likely, we were already at the right frame, and but it had already been decoded. Just try again, and this
				// time seek will go backwards and fix it
				return SeekFrame(frame);

			// Unknown error
			return false;
		}
	}

	do
	{
		// Need to read at least one frame to see what frame we are at. Then loop until we are at (or past)
		// the desired frame
		haveDecodedFrameToFetch = ReadVideoFrameBasic();
	} while (data->frame_number < frame);

	return haveDecodedFrameToFetch;
}

// Seeks to closest key frame at or BEFORE specified frame. Returns frame number if successful, or -1 if not.
Int64 VideoFileReader::SeekKeyFrame(Int64 frame)
{
	int success = libffmpeg::av_seek_frame(data->FormatContext, data->VideoStream->index,
		frame, AVSEEK_FLAG_BACKWARD);
	
	if (success < 0)
		return -1;

	ReadVideoFrameBasic();

	return data->frame_number;
}

// Reads next frame, decodes it into memory, and fetches it as a Bitmap.
// If a previous Seek() has read a frame, that will be the one returned (i.e.
// it will not do an additional read, since we already have a decoded frame in memory)
Bitmap^ VideoFileReader::ReadVideoFrame()
{
	if (haveDecodedFrameToFetch)
		// Already have decoded frame (e.g. from previous seek). Just copy to Bitmap
		return FetchVideoFrame();

	if (ReadVideoFrameBasic())
		// Decoded frame, now copy to managed Bitmap
		return FetchVideoFrame();
	else
		// EOF or error.
		return nullptr;
}

// Reads specified frame.
Bitmap^ VideoFileReader::ReadVideoFrame(Int64 frame)
{
	if (SeekFrame(frame))
		// Decoded frame, now copy to managed Bitmap
		return ReadVideoFrame();
	else
		// EOF or error.
		return nullptr;
}



// Read next video frame of the current video file
Int64 VideoFileReader::GetDts()
{
	return data->frame_number;
}

#if _DEBUG
// Read next video frame of the current video file
Int64 VideoFileReader::GetPts()
{
	return frameNumPts;
}
#endif

// Video frame has already been decoded, but will now be copied to managed Bitmap
Bitmap^ VideoFileReader::FetchVideoFrame( )
{
	if (!haveDecodedFrameToFetch)
		return nullptr;

	Bitmap^ bitmap = gcnew Bitmap(data->CodecContext->width, data->CodecContext->height, PixelFormat::Format24bppRgb);
	
	// lock the bitmap
	BitmapData^ bitmapData = bitmap->LockBits( System::Drawing::Rectangle( 0, 0, data->CodecContext->width, data->CodecContext->height ),
		ImageLockMode::ReadOnly, PixelFormat::Format24bppRgb );

	libffmpeg::uint8_t* ptr = reinterpret_cast<libffmpeg::uint8_t*>( static_cast<void*>( bitmapData->Scan0 ) );

	libffmpeg::uint8_t* srcData[4] = { ptr, NULL, NULL, NULL };
	int srcLinesize[4] = { bitmapData->Stride, 0, 0, 0 };

	// convert video frame to the RGB bitmap. This function can theoretically perform scaling, but the
	// source and destination sizes are always identical, so no actual scaling will occur.
	libffmpeg::sws_scale( data->ConvertContext, data->VideoFrame->data, data->VideoFrame->linesize, 0,
		data->CodecContext->height, srcData, srcLinesize );

	bitmap->UnlockBits( bitmapData );

	haveDecodedFrameToFetch = false;

	return bitmap;
}

} } }