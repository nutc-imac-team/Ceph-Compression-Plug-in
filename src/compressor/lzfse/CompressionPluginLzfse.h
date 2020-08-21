/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 BI SHUN KE <aionshun@livemail.tw>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_COMPRESSIONPLUGINLZFSE_H
#define CEPH_COMPRESSIONPLUGINLZFSE_H


#include "ceph_ver.h"
#include "compressor/CompressionPlugin.h"
#include "LzfseCompressor.h"

class CompressionPluginLzfse : public CompressionPlugin {
public:
    explicit CompressionPluginLzfse(CephContext *cct) : CompressionPlugin(cct)
    {}

    virtual int factory(CompressorRef *cs, std::ostream *ss)
    {
      if (compressor == nullptr) {
        LzfseCompressor *interface = new LzfseCompressor();
        compressor = CompressorRef(interface);
      }
      *cs = compressor;
      return 0;
    }
};

#endif
