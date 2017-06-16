// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/media_source_state.h"

#include <set>

#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "media/base/media_switches.h"
#include "media/base/media_track.h"
#include "media/base/media_tracks.h"
#include "media/base/mime_util.h"
#include "media/filters/chunk_demuxer.h"
#include "media/filters/frame_processor.h"
#include "media/filters/source_buffer_stream.h"

namespace media {

enum {
  // Limits the number of MEDIA_LOG() calls warning the user that a muxed stream
  // media segment is missing a block from at least one of the audio or video
  // tracks.
  kMaxMissingTrackInSegmentLogs = 10,
};

namespace {

TimeDelta EndTimestamp(const StreamParser::BufferQueue& queue) {
  return queue.back()->timestamp() + queue.back()->duration();
}

// Check the input |text_configs| and |bytestream_ids| and return false if
// duplicate track ids are detected.
bool CheckBytestreamTrackIds(
    const MediaTracks& tracks,
    const StreamParser::TextTrackConfigMap& text_configs) {
  std::set<StreamParser::TrackId> bytestream_ids;
  for (const auto& track : tracks.tracks()) {
    const StreamParser::TrackId& track_id = track->bytestream_track_id();
    if (bytestream_ids.find(track_id) != bytestream_ids.end()) {
      return false;
    }
    bytestream_ids.insert(track_id);
  }
  for (const auto& text_track : text_configs) {
    const StreamParser::TrackId& track_id = text_track.first;
    if (bytestream_ids.find(track_id) != bytestream_ids.end()) {
      return false;
    }
    bytestream_ids.insert(track_id);
  }
  return true;
}

}  // namespace

// List of time ranges for each SourceBuffer.
// static
Ranges<TimeDelta> MediaSourceState::ComputeRangesIntersection(
    const RangesList& active_ranges,
    bool ended) {
  // TODO(servolk): Perhaps this can be removed in favor of blink implementation
  // (MediaSource::buffered)? Currently this is only used on Android and for
  // updating DemuxerHost's buffered ranges during AppendData() as well as
  // SourceBuffer.buffered property implemetation.
  // Implementation of HTMLMediaElement.buffered algorithm in MSE spec.
  // https://dvcs.w3.org/hg/html-media/raw-file/default/media-source/media-source.html#dom-htmlmediaelement.buffered

  // Step 1: If activeSourceBuffers.length equals 0 then return an empty
  //  TimeRanges object and abort these steps.
  if (active_ranges.empty())
    return Ranges<TimeDelta>();

  // Step 2: Let active ranges be the ranges returned by buffered for each
  //  SourceBuffer object in activeSourceBuffers.
  // Step 3: Let highest end time be the largest range end time in the active
  //  ranges.
  TimeDelta highest_end_time;
  for (const auto& range : active_ranges) {
    if (!range.size())
      continue;

    highest_end_time = std::max(highest_end_time, range.end(range.size() - 1));
  }

  // Step 4: Let intersection ranges equal a TimeRange object containing a
  //  single range from 0 to highest end time.
  Ranges<TimeDelta> intersection_ranges;
  intersection_ranges.Add(TimeDelta(), highest_end_time);

  // Step 5: For each SourceBuffer object in activeSourceBuffers run the
  //  following steps:
  for (const auto& range : active_ranges) {
    // Step 5.1: Let source ranges equal the ranges returned by the buffered
    //  attribute on the current SourceBuffer.
    Ranges<TimeDelta> source_ranges = range;

    // Step 5.2: If readyState is "ended", then set the end time on the last
    //  range in source ranges to highest end time.
    if (ended && source_ranges.size()) {
      source_ranges.Add(source_ranges.start(source_ranges.size() - 1),
                        highest_end_time);
    }

    // Step 5.3: Let new intersection ranges equal the intersection between
    // the intersection ranges and the source ranges.
    // Step 5.4: Replace the ranges in intersection ranges with the new
    // intersection ranges.
    intersection_ranges = intersection_ranges.IntersectionWith(source_ranges);
  }

  return intersection_ranges;
}

MediaSourceState::MediaSourceState(
    std::unique_ptr<StreamParser> stream_parser,
    std::unique_ptr<FrameProcessor> frame_processor,
    const CreateDemuxerStreamCB& create_demuxer_stream_cb,
    const scoped_refptr<MediaLog>& media_log)
    : create_demuxer_stream_cb_(create_demuxer_stream_cb),
      timestamp_offset_during_append_(NULL),
      parsing_media_segment_(false),
      stream_parser_(stream_parser.release()),
      frame_processor_(frame_processor.release()),
      media_log_(media_log),
      state_(UNINITIALIZED),
      auto_update_timestamp_offset_(false) {
  DCHECK(!create_demuxer_stream_cb_.is_null());
  DCHECK(frame_processor_);
}

MediaSourceState::~MediaSourceState() {
  Shutdown();

  base::STLDeleteValues(&text_stream_map_);
}

void MediaSourceState::Init(
    const StreamParser::InitCB& init_cb,
    const std::string& expected_codecs,
    const StreamParser::EncryptedMediaInitDataCB& encrypted_media_init_data_cb,
    const NewTextTrackCB& new_text_track_cb) {
  DCHECK_EQ(state_, UNINITIALIZED);
  new_text_track_cb_ = new_text_track_cb;
  init_cb_ = init_cb;

  std::vector<std::string> expected_codecs_parsed;
  ParseCodecString(expected_codecs, &expected_codecs_parsed, false);

  std::vector<AudioCodec> expected_acodecs;
  std::vector<VideoCodec> expected_vcodecs;
  for (const auto& codec_id : expected_codecs_parsed) {
    AudioCodec acodec = StringToAudioCodec(codec_id);
    if (acodec != kUnknownAudioCodec) {
      expected_audio_codecs_.push_back(acodec);
      continue;
    }
    VideoCodec vcodec = StringToVideoCodec(codec_id);
    if (vcodec != kUnknownVideoCodec) {
      expected_video_codecs_.push_back(vcodec);
      continue;
    }
    MEDIA_LOG(INFO, media_log_) << "Unrecognized media codec: " << codec_id;
  }

  state_ = PENDING_PARSER_CONFIG;
  stream_parser_->Init(
      base::Bind(&MediaSourceState::OnSourceInitDone, base::Unretained(this)),
      base::Bind(&MediaSourceState::OnNewConfigs, base::Unretained(this),
                 expected_codecs),
      base::Bind(&MediaSourceState::OnNewBuffers, base::Unretained(this)),
      new_text_track_cb_.is_null(), encrypted_media_init_data_cb,
      base::Bind(&MediaSourceState::OnNewMediaSegment, base::Unretained(this)),
      base::Bind(&MediaSourceState::OnEndOfMediaSegment,
                 base::Unretained(this)),
      media_log_);
}

void MediaSourceState::SetSequenceMode(bool sequence_mode) {
  DCHECK(!parsing_media_segment_);

  frame_processor_->SetSequenceMode(sequence_mode);
}

void MediaSourceState::SetGroupStartTimestampIfInSequenceMode(
    base::TimeDelta timestamp_offset) {
  DCHECK(!parsing_media_segment_);

  frame_processor_->SetGroupStartTimestampIfInSequenceMode(timestamp_offset);
}

void MediaSourceState::SetTracksWatcher(
    const Demuxer::MediaTracksUpdatedCB& tracks_updated_cb) {
  DCHECK(init_segment_received_cb_.is_null());
  DCHECK(!tracks_updated_cb.is_null());
  init_segment_received_cb_ = tracks_updated_cb;
}

bool MediaSourceState::Append(const uint8_t* data,
                              size_t length,
                              TimeDelta append_window_start,
                              TimeDelta append_window_end,
                              TimeDelta* timestamp_offset) {
  append_in_progress_ = true;
  DCHECK(timestamp_offset);
  DCHECK(!timestamp_offset_during_append_);
  append_window_start_during_append_ = append_window_start;
  append_window_end_during_append_ = append_window_end;
  timestamp_offset_during_append_ = timestamp_offset;

  // TODO(wolenetz/acolwell): Curry and pass a NewBuffersCB here bound with
  // append window and timestamp offset pointer. See http://crbug.com/351454.
  bool result = stream_parser_->Parse(data, length);
  if (!result) {
    MEDIA_LOG(ERROR, media_log_)
        << __func__ << ": stream parsing failed. Data size=" << length
        << " append_window_start=" << append_window_start.InSecondsF()
        << " append_window_end=" << append_window_end.InSecondsF();
  }
  timestamp_offset_during_append_ = NULL;
  append_in_progress_ = false;
  return result;
}

void MediaSourceState::ResetParserState(TimeDelta append_window_start,
                                        TimeDelta append_window_end,
                                        base::TimeDelta* timestamp_offset) {
  DCHECK(timestamp_offset);
  DCHECK(!timestamp_offset_during_append_);
  timestamp_offset_during_append_ = timestamp_offset;
  append_window_start_during_append_ = append_window_start;
  append_window_end_during_append_ = append_window_end;

  stream_parser_->Flush();
  timestamp_offset_during_append_ = NULL;

  frame_processor_->Reset();
  parsing_media_segment_ = false;
  media_segment_has_data_for_track_.clear();
}

void MediaSourceState::Remove(TimeDelta start,
                              TimeDelta end,
                              TimeDelta duration) {
  for (const auto& it : audio_streams_) {
    it.second->Remove(start, end, duration);
  }

  for (const auto& it : video_streams_) {
    it.second->Remove(start, end, duration);
  }

  for (TextStreamMap::iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    itr->second->Remove(start, end, duration);
  }
}

bool MediaSourceState::EvictCodedFrames(DecodeTimestamp media_time,
                                        size_t newDataSize) {
  size_t total_buffered_size = 0;
  for (const auto& it : audio_streams_)
    total_buffered_size += it.second->GetBufferedSize();
  for (const auto& it : video_streams_)
    total_buffered_size += it.second->GetBufferedSize();
  for (const auto& it : text_stream_map_)
    total_buffered_size += it.second->GetBufferedSize();

  DVLOG(3) << __func__ << " media_time=" << media_time.InSecondsF()
           << " newDataSize=" << newDataSize
           << " total_buffered_size=" << total_buffered_size;

  if (total_buffered_size == 0)
    return true;

  bool success = true;
  for (const auto& it : audio_streams_) {
    uint64_t curr_size = it.second->GetBufferedSize();
    if (curr_size == 0)
      continue;
    uint64_t estimated_new_size = newDataSize * curr_size / total_buffered_size;
    DCHECK_LE(estimated_new_size, SIZE_MAX);
    success &= it.second->EvictCodedFrames(
        media_time, static_cast<size_t>(estimated_new_size));
  }
  for (const auto& it : video_streams_) {
    uint64_t curr_size = it.second->GetBufferedSize();
    if (curr_size == 0)
      continue;
    uint64_t estimated_new_size = newDataSize * curr_size / total_buffered_size;
    DCHECK_LE(estimated_new_size, SIZE_MAX);
    success &= it.second->EvictCodedFrames(
        media_time, static_cast<size_t>(estimated_new_size));
  }
  for (const auto& it : text_stream_map_) {
    uint64_t curr_size = it.second->GetBufferedSize();
    if (curr_size == 0)
      continue;
    uint64_t estimated_new_size = newDataSize * curr_size / total_buffered_size;
    DCHECK_LE(estimated_new_size, SIZE_MAX);
    success &= it.second->EvictCodedFrames(
        media_time, static_cast<size_t>(estimated_new_size));
  }

  DVLOG(3) << __func__ << " success=" << success;
  return success;
}

Ranges<TimeDelta> MediaSourceState::GetBufferedRanges(TimeDelta duration,
                                                      bool ended) const {
  RangesList ranges_list;
  for (const auto& it : audio_streams_)
    ranges_list.push_back(it.second->GetBufferedRanges(duration));

  for (const auto& it : video_streams_)
    ranges_list.push_back(it.second->GetBufferedRanges(duration));

  for (TextStreamMap::const_iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    ranges_list.push_back(itr->second->GetBufferedRanges(duration));
  }

  return ComputeRangesIntersection(ranges_list, ended);
}

TimeDelta MediaSourceState::GetHighestPresentationTimestamp() const {
  TimeDelta max_pts;

  for (const auto& it : audio_streams_) {
    max_pts = std::max(max_pts, it.second->GetHighestPresentationTimestamp());
  }

  for (const auto& it : video_streams_) {
    max_pts = std::max(max_pts, it.second->GetHighestPresentationTimestamp());
  }

  for (TextStreamMap::const_iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    max_pts = std::max(max_pts, itr->second->GetHighestPresentationTimestamp());
  }

  return max_pts;
}

TimeDelta MediaSourceState::GetMaxBufferedDuration() const {
  TimeDelta max_duration;

  for (const auto& it : audio_streams_) {
    max_duration = std::max(max_duration, it.second->GetBufferedDuration());
  }

  for (const auto& it : video_streams_) {
    max_duration = std::max(max_duration, it.second->GetBufferedDuration());
  }

  for (TextStreamMap::const_iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    max_duration = std::max(max_duration, itr->second->GetBufferedDuration());
  }

  return max_duration;
}

void MediaSourceState::StartReturningData() {
  for (const auto& it : audio_streams_) {
    it.second->StartReturningData();
  }

  for (const auto& it : video_streams_) {
    it.second->StartReturningData();
  }

  for (TextStreamMap::iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    itr->second->StartReturningData();
  }
}

void MediaSourceState::AbortReads() {
  for (const auto& it : audio_streams_) {
    it.second->AbortReads();
  }

  for (const auto& it : video_streams_) {
    it.second->AbortReads();
  }

  for (TextStreamMap::iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    itr->second->AbortReads();
  }
}

void MediaSourceState::Seek(TimeDelta seek_time) {
  for (const auto& it : audio_streams_) {
    it.second->Seek(seek_time);
  }

  for (const auto& it : video_streams_) {
    it.second->Seek(seek_time);
  }

  for (TextStreamMap::iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    itr->second->Seek(seek_time);
  }
}

void MediaSourceState::CompletePendingReadIfPossible() {
  for (const auto& it : audio_streams_) {
    it.second->CompletePendingReadIfPossible();
  }

  for (const auto& it : video_streams_) {
    it.second->CompletePendingReadIfPossible();
  }

  for (TextStreamMap::iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    itr->second->CompletePendingReadIfPossible();
  }
}

void MediaSourceState::OnSetDuration(TimeDelta duration) {
  for (const auto& it : audio_streams_) {
    it.second->OnSetDuration(duration);
  }

  for (const auto& it : video_streams_) {
    it.second->OnSetDuration(duration);
  }

  for (TextStreamMap::iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    itr->second->OnSetDuration(duration);
  }
}

void MediaSourceState::MarkEndOfStream() {
  for (const auto& it : audio_streams_) {
    it.second->MarkEndOfStream();
  }

  for (const auto& it : video_streams_) {
    it.second->MarkEndOfStream();
  }

  for (TextStreamMap::iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    itr->second->MarkEndOfStream();
  }
}

void MediaSourceState::UnmarkEndOfStream() {
  for (const auto& it : audio_streams_) {
    it.second->UnmarkEndOfStream();
  }

  for (const auto& it : video_streams_) {
    it.second->UnmarkEndOfStream();
  }

  for (TextStreamMap::iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    itr->second->UnmarkEndOfStream();
  }
}

void MediaSourceState::Shutdown() {
  for (const auto& it : audio_streams_) {
    it.second->Shutdown();
  }

  for (const auto& it : video_streams_) {
    it.second->Shutdown();
  }

  for (TextStreamMap::iterator itr = text_stream_map_.begin();
       itr != text_stream_map_.end(); ++itr) {
    itr->second->Shutdown();
  }
}

void MediaSourceState::SetMemoryLimits(DemuxerStream::Type type,
                                       size_t memory_limit) {
  switch (type) {
    case DemuxerStream::AUDIO:
      for (const auto& it : audio_streams_) {
        it.second->SetStreamMemoryLimit(memory_limit);
      }
      break;
    case DemuxerStream::VIDEO:
      for (const auto& it : video_streams_) {
        it.second->SetStreamMemoryLimit(memory_limit);
      }
      break;
    case DemuxerStream::TEXT:
      for (TextStreamMap::iterator itr = text_stream_map_.begin();
           itr != text_stream_map_.end(); ++itr) {
        itr->second->SetStreamMemoryLimit(memory_limit);
      }
      break;
    case DemuxerStream::UNKNOWN:
    case DemuxerStream::NUM_TYPES:
      NOTREACHED();
      break;
  }
}

bool MediaSourceState::IsSeekWaitingForData() const {
  for (const auto& it : audio_streams_) {
    if (it.second->IsSeekWaitingForData())
      return true;
  }

  for (const auto& it : video_streams_) {
    if (it.second->IsSeekWaitingForData())
      return true;
  }

  // NOTE: We are intentionally not checking the text tracks
  // because text tracks are discontinuous and may not have data
  // for the seek position. This is ok and playback should not be
  // stalled because we don't have cues. If cues, with timestamps after
  // the seek time, eventually arrive they will be delivered properly
  // in response to ChunkDemuxerStream::Read() calls.

  return false;
}

bool MediaSourceState::OnNewConfigs(
    std::string expected_codecs,
    std::unique_ptr<MediaTracks> tracks,
    const StreamParser::TextTrackConfigMap& text_configs) {
  DCHECK(tracks.get());
  DVLOG(1) << __func__ << " expected_codecs=" << expected_codecs
           << " tracks=" << tracks->tracks().size();
  DCHECK_GE(state_, PENDING_PARSER_CONFIG);

  // Check that there is no clashing bytestream track ids.
  if (!CheckBytestreamTrackIds(*tracks, text_configs)) {
    MEDIA_LOG(ERROR, media_log_) << "Duplicate bytestream track ids detected";
    for (const auto& track : tracks->tracks()) {
      const StreamParser::TrackId& track_id = track->bytestream_track_id();
      MEDIA_LOG(DEBUG, media_log_) << TrackTypeToStr(track->type()) << " track "
                                   << " bytestream track id=" << track_id;
    }
    return false;
  }

  // MSE spec allows new configs to be emitted only during Append, but not
  // during Flush or parser reset operations.
  CHECK(append_in_progress_);

  bool success = true;

  // TODO(wolenetz): Update codec string strictness, if necessary, once spec
  // issue https://github.com/w3c/media-source/issues/161 is resolved.
  std::vector<AudioCodec> expected_acodecs = expected_audio_codecs_;
  std::vector<VideoCodec> expected_vcodecs = expected_video_codecs_;

  for (const auto& track : tracks->tracks()) {
    const auto& track_id = track->bytestream_track_id();

    if (track->type() == MediaTrack::Audio) {
      AudioDecoderConfig audio_config = tracks->getAudioConfig(track_id);
      DVLOG(1) << "Audio track_id=" << track_id
               << " config: " << audio_config.AsHumanReadableString();
      DCHECK(audio_config.IsValidConfig());

      const auto& it = std::find(expected_acodecs.begin(),
                                 expected_acodecs.end(), audio_config.codec());
      if (it == expected_acodecs.end()) {
        MEDIA_LOG(ERROR, media_log_) << "Audio stream codec "
                                     << GetCodecName(audio_config.codec())
                                     << " doesn't match SourceBuffer codecs.";
        return false;
      }
      expected_acodecs.erase(it);

      ChunkDemuxerStream* stream = nullptr;
      if (!first_init_segment_received_) {
        DCHECK(audio_streams_.find(track_id) == audio_streams_.end());
        stream = create_demuxer_stream_cb_.Run(DemuxerStream::AUDIO);
        if (!stream || !frame_processor_->AddTrack(track_id, stream)) {
          MEDIA_LOG(ERROR, media_log_) << "Failed to create audio stream.";
          return false;
        }
        audio_streams_[track_id] = stream;
        media_log_->SetBooleanProperty("found_audio_stream", true);
        media_log_->SetStringProperty("audio_codec_name",
                                      GetCodecName(audio_config.codec()));
      } else {
        if (audio_streams_.size() > 1) {
          auto it = audio_streams_.find(track_id);
          if (it != audio_streams_.end())
            stream = it->second;
        } else {
          // If there is only one audio track then bytestream id might change in
          // a new init segment. So update our state and nofity frame processor.
          const auto& it = audio_streams_.begin();
          if (it != audio_streams_.end()) {
            stream = it->second;
            if (it->first != track_id) {
              frame_processor_->UpdateTrack(it->first, track_id);
              audio_streams_[track_id] = stream;
              audio_streams_.erase(it->first);
            }
          }
        }
        if (!stream) {
          MEDIA_LOG(ERROR, media_log_) << "Got unexpected audio track"
                                       << " track_id=" << track_id;
          return false;
        }
      }

      track->set_id(stream->media_track_id());
      frame_processor_->OnPossibleAudioConfigUpdate(audio_config);
      success &= stream->UpdateAudioConfig(audio_config, media_log_);
    } else if (track->type() == MediaTrack::Video) {
      VideoDecoderConfig video_config = tracks->getVideoConfig(track_id);
      DVLOG(1) << "Video track_id=" << track_id
               << " config: " << video_config.AsHumanReadableString();
      DCHECK(video_config.IsValidConfig());

      const auto& it = std::find(expected_vcodecs.begin(),
                                 expected_vcodecs.end(), video_config.codec());
      if (it == expected_vcodecs.end()) {
        MEDIA_LOG(ERROR, media_log_) << "Video stream codec "
                                     << GetCodecName(video_config.codec())
                                     << " doesn't match SourceBuffer codecs.";
        return false;
      }
      expected_vcodecs.erase(it);

      ChunkDemuxerStream* stream = nullptr;
      if (!first_init_segment_received_) {
        DCHECK(video_streams_.find(track_id) == video_streams_.end());
        stream = create_demuxer_stream_cb_.Run(DemuxerStream::VIDEO);
        if (!stream || !frame_processor_->AddTrack(track_id, stream)) {
          MEDIA_LOG(ERROR, media_log_) << "Failed to create video stream.";
          return false;
        }
        video_streams_[track_id] = stream;
        media_log_->SetBooleanProperty("found_video_stream", true);
        media_log_->SetStringProperty("video_codec_name",
                                      GetCodecName(video_config.codec()));
      } else {
        if (video_streams_.size() > 1) {
          auto it = video_streams_.find(track_id);
          if (it != video_streams_.end())
            stream = it->second;
        } else {
          // If there is only one video track then bytestream id might change in
          // a new init segment. So update our state and nofity frame processor.
          const auto& it = video_streams_.begin();
          if (it != video_streams_.end()) {
            stream = it->second;
            if (it->first != track_id) {
              frame_processor_->UpdateTrack(it->first, track_id);
              video_streams_[track_id] = stream;
              video_streams_.erase(it->first);
            }
          }
        }
        if (!stream) {
          MEDIA_LOG(ERROR, media_log_) << "Got unexpected video track"
                                       << " track_id=" << track_id;
          return false;
        }
      }

      track->set_id(stream->media_track_id());
      success &= stream->UpdateVideoConfig(video_config, media_log_);
    } else {
      MEDIA_LOG(ERROR, media_log_) << "Error: unsupported media track type "
                                   << track->type();
      return false;
    }
  }

  if (!expected_acodecs.empty() || !expected_vcodecs.empty()) {
    for (const auto& acodec : expected_acodecs) {
      MEDIA_LOG(ERROR, media_log_) << "Initialization segment misses expected "
                                   << GetCodecName(acodec) << " track.";
    }
    for (const auto& vcodec : expected_vcodecs) {
      MEDIA_LOG(ERROR, media_log_) << "Initialization segment misses expected "
                                   << GetCodecName(vcodec) << " track.";
    }
    return false;
  }

  typedef StreamParser::TextTrackConfigMap::const_iterator TextConfigItr;
  if (text_stream_map_.empty()) {
    for (TextConfigItr itr = text_configs.begin(); itr != text_configs.end();
         ++itr) {
      ChunkDemuxerStream* const text_stream =
          create_demuxer_stream_cb_.Run(DemuxerStream::TEXT);
      if (!frame_processor_->AddTrack(itr->first, text_stream)) {
        success &= false;
        MEDIA_LOG(ERROR, media_log_) << "Failed to add text track ID "
                                     << itr->first << " to frame processor.";
        break;
      }
      text_stream->UpdateTextConfig(itr->second, media_log_);
      text_stream_map_[itr->first] = text_stream;
      new_text_track_cb_.Run(text_stream, itr->second);
    }
  } else {
    const size_t text_count = text_stream_map_.size();
    if (text_configs.size() != text_count) {
      success &= false;
      MEDIA_LOG(ERROR, media_log_)
          << "The number of text track configs changed.";
    } else if (text_count == 1) {
      TextConfigItr config_itr = text_configs.begin();
      TextStreamMap::iterator stream_itr = text_stream_map_.begin();
      ChunkDemuxerStream* text_stream = stream_itr->second;
      TextTrackConfig old_config = text_stream->text_track_config();
      TextTrackConfig new_config(
          config_itr->second.kind(), config_itr->second.label(),
          config_itr->second.language(), old_config.id());
      if (!new_config.Matches(old_config)) {
        success &= false;
        MEDIA_LOG(ERROR, media_log_)
            << "New text track config does not match old one.";
      } else {
        StreamParser::TrackId old_id = stream_itr->first;
        StreamParser::TrackId new_id = config_itr->first;
        if (new_id != old_id) {
          if (frame_processor_->UpdateTrack(old_id, new_id)) {
            text_stream_map_.clear();
            text_stream_map_[config_itr->first] = text_stream;
          } else {
            success &= false;
            MEDIA_LOG(ERROR, media_log_)
                << "Error remapping single text track number";
          }
        }
      }
    } else {
      for (TextConfigItr config_itr = text_configs.begin();
           config_itr != text_configs.end(); ++config_itr) {
        TextStreamMap::iterator stream_itr =
            text_stream_map_.find(config_itr->first);
        if (stream_itr == text_stream_map_.end()) {
          success &= false;
          MEDIA_LOG(ERROR, media_log_)
              << "Unexpected text track configuration for track ID "
              << config_itr->first;
          break;
        }

        const TextTrackConfig& new_config = config_itr->second;
        ChunkDemuxerStream* stream = stream_itr->second;
        TextTrackConfig old_config = stream->text_track_config();
        if (!new_config.Matches(old_config)) {
          success &= false;
          MEDIA_LOG(ERROR, media_log_) << "New text track config for track ID "
                                       << config_itr->first
                                       << " does not match old one.";
          break;
        }
      }
    }
  }

  if (audio_streams_.empty() && video_streams_.empty()) {
    DVLOG(1) << __func__ << ": couldn't find a valid audio or video stream";
    return false;
  }

  frame_processor_->SetAllTrackBuffersNeedRandomAccessPoint();

  if (!first_init_segment_received_) {
    first_init_segment_received_ = true;
    SetStreamMemoryLimits();
  }

  DVLOG(1) << "OnNewConfigs() : " << (success ? "success" : "failed");
  if (success) {
    if (state_ == PENDING_PARSER_CONFIG)
      state_ = PENDING_PARSER_INIT;
    DCHECK(!init_segment_received_cb_.is_null());
    init_segment_received_cb_.Run(std::move(tracks));
  }

  return success;
}

void MediaSourceState::SetStreamMemoryLimits() {
  auto cmd_line = base::CommandLine::ForCurrentProcess();

  std::string audio_buf_limit_switch =
      cmd_line->GetSwitchValueASCII(switches::kMSEAudioBufferSizeLimit);
  unsigned audio_buf_size_limit = 0;
  if (base::StringToUint(audio_buf_limit_switch, &audio_buf_size_limit) &&
      audio_buf_size_limit > 0) {
    MEDIA_LOG(INFO, media_log_)
        << "Custom audio per-track SourceBuffer size limit="
        << audio_buf_size_limit;
    for (const auto& it : audio_streams_) {
      it.second->SetStreamMemoryLimit(audio_buf_size_limit);
    }
  }

  std::string video_buf_limit_switch =
      cmd_line->GetSwitchValueASCII(switches::kMSEVideoBufferSizeLimit);
  unsigned video_buf_size_limit = 0;
  if (base::StringToUint(video_buf_limit_switch, &video_buf_size_limit) &&
      video_buf_size_limit > 0) {
    MEDIA_LOG(INFO, media_log_)
        << "Custom video per-track SourceBuffer size limit="
        << video_buf_size_limit;
    for (const auto& it : video_streams_) {
      it.second->SetStreamMemoryLimit(video_buf_size_limit);
    }
  }
}

void MediaSourceState::OnNewMediaSegment() {
  DVLOG(2) << "OnNewMediaSegment()";
  DCHECK_EQ(state_, PARSER_INITIALIZED);
  parsing_media_segment_ = true;
  media_segment_has_data_for_track_.clear();
}

void MediaSourceState::OnEndOfMediaSegment() {
  DVLOG(2) << "OnEndOfMediaSegment()";
  DCHECK_EQ(state_, PARSER_INITIALIZED);
  parsing_media_segment_ = false;

  for (const auto& it : audio_streams_) {
    if (!media_segment_has_data_for_track_[it.first]) {
      LIMITED_MEDIA_LOG(DEBUG, media_log_, num_missing_track_logs_,
                        kMaxMissingTrackInSegmentLogs)
          << "Media segment did not contain any coded frames for track "
          << it.first << ", mismatching initialization segment. Therefore, MSE"
                         " coded frame processing may not interoperably detect"
                         " discontinuities in appended media.";
    }
  }
  for (const auto& it : video_streams_) {
    if (!media_segment_has_data_for_track_[it.first]) {
      LIMITED_MEDIA_LOG(DEBUG, media_log_, num_missing_track_logs_,
                        kMaxMissingTrackInSegmentLogs)
          << "Media segment did not contain any coded frames for track "
          << it.first << ", mismatching initialization segment. Therefore, MSE"
                         " coded frame processing may not interoperably detect"
                         " discontinuities in appended media.";
    }
  }
}

bool MediaSourceState::OnNewBuffers(
    const StreamParser::BufferQueueMap& buffer_queue_map) {
  DVLOG(2) << __func__ << " buffer_queues=" << buffer_queue_map.size();
  DCHECK_EQ(state_, PARSER_INITIALIZED);
  DCHECK(timestamp_offset_during_append_);
  DCHECK(parsing_media_segment_);

  for (const auto& it : buffer_queue_map) {
    const StreamParser::BufferQueue& bufq = it.second;
    DCHECK(!bufq.empty());
    media_segment_has_data_for_track_[it.first] = true;
  }

  const TimeDelta timestamp_offset_before_processing =
      *timestamp_offset_during_append_;

  // Calculate the new timestamp offset for audio/video tracks if the stream
  // parser has requested automatic updates.
  TimeDelta new_timestamp_offset = timestamp_offset_before_processing;
  if (auto_update_timestamp_offset_) {
    TimeDelta min_end_timestamp = kNoTimestamp;
    for (const auto& it : buffer_queue_map) {
      const StreamParser::BufferQueue& bufq = it.second;
      DCHECK(!bufq.empty());
      if (min_end_timestamp == kNoTimestamp ||
          EndTimestamp(bufq) < min_end_timestamp) {
        min_end_timestamp = EndTimestamp(bufq);
        DCHECK_NE(kNoTimestamp, min_end_timestamp);
      }
    }
    if (min_end_timestamp != kNoTimestamp)
      new_timestamp_offset += min_end_timestamp;
  }

  if (!frame_processor_->ProcessFrames(
          buffer_queue_map, append_window_start_during_append_,
          append_window_end_during_append_, timestamp_offset_during_append_)) {
    return false;
  }

  // Only update the timestamp offset if the frame processor hasn't already.
  if (auto_update_timestamp_offset_ &&
      timestamp_offset_before_processing == *timestamp_offset_during_append_) {
    *timestamp_offset_during_append_ = new_timestamp_offset;
  }

  return true;
}
void MediaSourceState::OnSourceInitDone(
    const StreamParser::InitParameters& params) {
  DCHECK_EQ(state_, PENDING_PARSER_INIT);
  state_ = PARSER_INITIALIZED;
  auto_update_timestamp_offset_ = params.auto_update_timestamp_offset;
  base::ResetAndReturn(&init_cb_).Run(params);
}

}  // namespace media