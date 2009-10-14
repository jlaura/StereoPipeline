// __BEGIN_LICENSE__
//
// Copyright (C) 2008 United States Government as represented by the
// Administrator of the National Aeronautics and Space Administration
// (NASA).  All Rights Reserved.
//
// Copyright 2008 Carnegie Mellon University. All rights reserved.
//
// This software is distributed under the NASA Open Source Agreement
// (NOSA), version 1.3.  The NOSA has been approved by the Open Source
// Initiative.  See the file COPYING at the top of the distribution
// directory tree for the complete NOSA document.
//
// THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF ANY
// KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT
// LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
// SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT
// THE SUBJECT SOFTWARE WILL BE ERROR FREE, OR ANY WARRANTY THAT
// DOCUMENTATION, IF PROVIDED, WILL CONFORM TO THE SUBJECT SOFTWARE.
//
// __END_LICENSE__

/// \file stereo.cc
///

#ifndef __ASP_STEREO_H__
#define __ASP_STEREO_H__

#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/fstream.hpp>

namespace po = boost::program_options;
namespace fs = boost::filesystem;

#include <vw/Core.h>
#include <vw/Image.h>
#include <vw/Math.h>
#include <vw/FileIO.h>
#include <vw/Camera.h>
#include <vw/Stereo.h>
#include <vw/Cartography.h>
#include <vw/Math/EulerAngles.h>
using namespace vw;
using namespace vw::math;
using namespace vw::camera;

#include <asp/Sessions.h>
#include <asp/Core/StereoSettings.h>
#include <asp/Core/BundleAdjustUtils.h>
#include <asp/Core/MedianFilter.h>
#include <asp/Core/BlobIndexThreaded.h>
#include <asp/Core/InpaintView.h>

// Support for ISIS image files
#if defined(ASP_HAVE_PKG_ISIS) && ASP_HAVE_PKG_ISIS == 1
#include <asp/IsisIO/DiskImageResourceIsis.h>
#endif

// Boost headers
#include <boost/thread/xtime.hpp>
// Posix time is not fully supported in the version of Boost for RHEL
// Workstation 4
#ifdef __APPLE__
#include <boost/date_time/posix_time/posix_time.hpp>
#else
#include <ctime>
#endif

// The stereo pipeline has several stages, which are enumerated below.
enum { PREPROCESSING = 0,
       CORRELATION,
       REFINEMENT,
       FILTERING,
       POINT_CLOUD,
       WIRE_MESH,
       NUM_STAGES};

// Allows FileIO to correctly read/write these pixel types
namespace vw {
  template<> struct PixelFormatID<Vector3>   { static const PixelFormatEnum value = VW_PIXEL_GENERIC_3_CHANNEL; };
}

// Duplicate matches for any given interest point probably indicate a
// poor match, so we cull those out here.
void remove_duplicates(std::vector<ip::InterestPoint> &ip1,
                       std::vector<ip::InterestPoint> &ip2) {
  std::vector<ip::InterestPoint> new_ip1, new_ip2;

  for (unsigned i = 0; i < ip1.size(); ++i) {
    bool bad_entry = false;
    for (unsigned j = 0; j < ip1.size(); ++j) {
      if (i != j &&
          ((ip1[i].x == ip1[j].x && ip1[i].y == ip1[j].y) ||
           (ip2[i].x == ip2[j].x && ip2[i].y == ip2[j].y)) ) {
        bad_entry = true;
      }
    }
    if (!bad_entry) {
      new_ip1.push_back(ip1[i]);
      new_ip2.push_back(ip2[i]);
    }
  }

  ip1 = new_ip1;
  ip2 = new_ip2;
}

static std::string prefix_from_filename(std::string const& filename) {
  std::string result = filename;
  int index = result.rfind(".");
  if (index != -1)
    result.erase(index, result.size());
  return result;
}

// Posix time is not fully supported in the version of Boost for RHEL
// Workstation 4
#ifndef __APPLE__
inline std::string
current_posix_time_string()
{
  char time_string[2048];
  time_t t = time(0);
  struct tm* time_struct = localtime(&t);
  strftime(time_string, 2048, "%F %T", time_struct);
  return std::string(time_string);
}
#else
inline std::string
current_posix_time_string()
{
  std::ostringstream time_string_stream;
  time_string_stream << boost::posix_time::second_clock::local_time();
  return time_string_stream.str();
}
#endif

// approximate search range
//  Find interest points and grow them into a search range
BBox2i
approximate_search_range( std::string left_image,
                          std::string right_image,
                          float scale ) {

  vw_out(0) << "\t--> Using interest points to determine search window.\n";
  std::vector<ip::InterestPoint> matched_ip1, matched_ip2;
  float i_scale = 1.0/scale;

  // String names
  std::string left_ip_file =
    prefix_from_filename( left_image ) + ".vwip";
  std::string right_ip_file =
    prefix_from_filename( right_image ) + ".vwip";
  std::string match_file =
    prefix_from_filename( left_image ) + "__" +
    prefix_from_filename( right_image ) + ".match";

  // Building / Loading Interest point data
  if ( fs::exists(match_file) ) {

    vw_out(0) << "\t    * Using cached match file.\n";
    ip::read_binary_match_file(match_file, matched_ip1, matched_ip2);

  } else {

    std::vector<ip::InterestPoint> ip1_copy, ip2_copy;

    if ( !fs::exists(left_ip_file) ||
         !fs::exists(right_ip_file) ) {

      // Worst case, no interest point operations have been performed before
      vw_out(0) << "\t    * Locating Interest Points\n";
      DiskImageView<PixelGray<float32> > left_sub_disk_image(left_image);
      DiskImageView<PixelGray<float32> > right_sub_disk_image(right_image);
      ImageViewRef<PixelGray<float32> > left_sub_image = left_sub_disk_image;
      ImageViewRef<PixelGray<float32> > right_sub_image = right_sub_disk_image;

      // Interest Point module detector code.
      ip::LogInterestOperator log_detector;
      ip::ScaledInterestPointDetector<ip::LogInterestOperator> detector(log_detector, 500);
      std::list<ip::InterestPoint> ip1, ip2;
      vw_out(0) << "\t    * Processing " << left_image << "...\n" << std::flush;
      ip1 = detect_interest_points( left_sub_image, detector );
      vw_out(0) << "Located " << ip1.size() << " points.\n";
      vw_out(0) << "\t    * Processing " << right_image << "...\n" << std::flush;
      ip2 = detect_interest_points( right_sub_image, detector );
      vw_out(0) << "Located " << ip2.size() << " points.\n";

      vw_out(0) << "\t    * Generating descriptors..." << std::flush;
      ip::PatchDescriptorGenerator descriptor;
      descriptor( left_sub_image, ip1 );
      descriptor( right_sub_image, ip2 );
      vw_out(0) << "done.\n";

      // Writing out the results
      vw_out(0) << "\t    * Caching interest points: "
                << left_ip_file << " & " << right_ip_file << std::endl;
      ip::write_binary_ip_file( left_ip_file, ip1 );
      ip::write_binary_ip_file( right_ip_file, ip2 );

    }

    vw_out(0) << "\t    * Using cached IPs.\n";
    ip1_copy = ip::read_binary_ip_file(left_ip_file);
    ip2_copy = ip::read_binary_ip_file(right_ip_file);

    vw_out(0) << "\t    * Matching interest points\n";
    ip::InterestPointMatcher<ip::L2NormMetric,ip::NullConstraint> matcher(0.8);

    matcher(ip1_copy, ip2_copy, matched_ip1, matched_ip2,
            false, TerminalProgressCallback( InfoMessage, "\t    Matching: "));
    vw_out(InfoMessage) << "\t    " << matched_ip1.size() << " putative matches.\n";

    vw_out(0) << "\t    * Rejecting outliers using RANSAC.\n";
    remove_duplicates(matched_ip1, matched_ip2);
    std::vector<Vector3> ransac_ip1 = ip::iplist_to_vectorlist(matched_ip1);
    std::vector<Vector3> ransac_ip2 = ip::iplist_to_vectorlist(matched_ip2);
    std::vector<int> indices;

    try {
      Matrix<double> trans;
      math::RandomSampleConsensus<math::TranslationRotationFittingFunctor,math::InterestPointErrorMetric>
        ransac( math::TranslationRotationFittingFunctor(), math::InterestPointErrorMetric(), 25 );
      trans = ransac( ransac_ip1, ransac_ip2 );
      vw_out(DebugMessage) << "\t    * Ransac Result: " << trans << std::endl;
      indices = ransac.inlier_indices(trans, ransac_ip1, ransac_ip2 );
    } catch (...) {
      vw_out(0) << "-------------------------------WARNING---------------------------------\n";
      vw_out(0) << "\t    RANSAC failed! Unable to auto detect search range.\n\n";
      vw_out(0) << "\t    Please proceed cautiously!\n";
      vw_out(0) << "-------------------------------WARNING---------------------------------\n";
      return BBox2i(-10,-10,20,20);
    }

    { // Keeping only inliers
      std::vector<ip::InterestPoint> inlier_ip1, inlier_ip2;
      for ( unsigned i = 0; i < indices.size(); i++ ) {
        inlier_ip1.push_back( matched_ip1[indices[i]] );
        inlier_ip2.push_back( matched_ip2[indices[i]] );
      }
      matched_ip1 = inlier_ip1;
      matched_ip2 = inlier_ip2;
    }

    vw_out(0) << "\t    * Caching matches: " << match_file << "\n";
    write_binary_match_file( match_file, matched_ip1, matched_ip2);
  }

  // Find search window based on interest point matches
  BBox2i search_range;
  for (unsigned i = 0; i < matched_ip1.size(); i++) {
    Vector2i translation = ( i_scale*Vector2i(matched_ip2[i].x, matched_ip2[i].y) -
                             i_scale*Vector2i(matched_ip1[i].x, matched_ip1[i].y) );

    if ( i == 0 ) {
      search_range.min() = translation;
      search_range.max() = translation + Vector2i(1,1);
    } else
      search_range.grow( translation );
  }
  Vector2i offset = search_range.size()/4; // So we can grow by 50%
  search_range.min() -= offset;
  search_range.max() += offset;

  vw_out(0) << "\t--> Dectected search range: " << search_range << "\n";
  return search_range;
}

#endif//__ASP_STEREO_H__