#pragma once

#include "../read_pipeline/ReadPipeline.h"

// Given a read with unstitched chunks, stitch the chunks (accounting for overlap) and assign basecalled read and
// qstring to Read
void stitch_chunks(std::shared_ptr<Read> read);
