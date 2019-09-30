#pragma once

#include "FileSystem.h"
#include <AK/ByteBuffer.h>

class DiskCache;

class DiskBackedFS : public FS {
public:
    virtual ~DiskBackedFS() override;

    virtual bool is_disk_backed() const override { return true; }

    DiskDevice& device() { return *m_device; }
    const DiskDevice& device() const { return *m_device; }

    virtual void flush_writes() override;

protected:
    explicit DiskBackedFS(NonnullRefPtr<DiskDevice>&&);

    ByteBuffer read_block(unsigned index) const;
    ByteBuffer read_blocks(unsigned index, unsigned count) const;

    bool write_block(unsigned index, const ByteBuffer&);
    bool write_blocks(unsigned index, unsigned count, const ByteBuffer&);

private:
    DiskCache& cache() const;

    NonnullRefPtr<DiskDevice> m_device;
    mutable OwnPtr<DiskCache> m_cache;
};
