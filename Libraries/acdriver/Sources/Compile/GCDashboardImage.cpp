/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree. An additional grant
 of patent rights can be found in the PATENTS file in the same directory.
 */

#include <acdriver/Compile/GCDashboardImage.h>
#include <acdriver/CompileOutput.h>
#include <acdriver/Result.h>
#include <libutil/Filesystem.h>

using acdriver::Compile::GCDashboardImage;
using acdriver::CompileOutput;
using acdriver::Result;
using libutil::Filesystem;

bool GCDashboardImage::
Compile(
    std::shared_ptr<xcassets::Asset::GCDashboardImage> const &gcDashboardImage,
    Filesystem *filesystem,
    CompileOutput *compileOutput,
    Result *result)
{
    result->document(
        Result::Severity::Warning,
        gcDashboardImage->path(),
        { CompileOutput::AssetReference(gcDashboardImage) },
        "Not Implemented",
        "gc dashboard image not yet supported");

    return false;
}
