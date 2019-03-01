
#include <type_traits>
#include "kernel.hpp"
#include <frg/container_of.hpp>
#include "types.hpp"

namespace thor {

PhysicalAddr MemoryBundle::blockForRange(uintptr_t) {
	assert(!"This function is not supported anymore. Convert code to fetchRange");
	__builtin_unreachable();
}

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

bool Memory::transfer(TransferNode *node) {
	node->_progress = 0;

	struct Ops {
		static bool process(TransferNode *node) {
			while(node->_progress < node->_size) {
				if(!prepareDestAndCopy(node))
					return false;
			}

			return true;
		}

		static bool prepareDestAndCopy(TransferNode *node) {
			auto dest_misalign = (node->_destOffset + node->_progress) % kPageSize;

			node->_worklet.setup(&Ops::fetchedDest);
			node->_destFetch.setup(&node->_worklet);
			if(!node->_destBundle->fetchRange(node->_destOffset + node->_progress - dest_misalign,
					&node->_destFetch))
				return false;
			return prepareSrcAndCopy(node);
		}

		static bool prepareSrcAndCopy(TransferNode *node) {
			auto src_misalign = (node->_srcOffset + node->_progress) % kPageSize;

			node->_worklet.setup(&Ops::fetchedSrc);
			node->_srcFetch.setup(&node->_worklet);
			if(!node->_srcBundle->fetchRange(node->_srcOffset + node->_progress - src_misalign,
					&node->_srcFetch))
				return false;
			return doCopy(node);
		}

		static bool doCopy(TransferNode *node) {
			auto dest_misalign = (node->_destOffset + node->_progress) % kPageSize;
			auto src_misalign = (node->_srcOffset + node->_progress) % kPageSize;
			size_t chunk = frigg::min(frigg::min(kPageSize - dest_misalign,
					kPageSize - src_misalign), node->_size - node->_progress);

			auto dest_page = node->_destFetch.range().get<0>();
			auto src_page = node->_srcFetch.range().get<0>();
			assert(dest_page != PhysicalAddr(-1));
			assert(src_page != PhysicalAddr(-1));

			PageAccessor dest_accessor{dest_page};
			PageAccessor src_accessor{src_page};
			memcpy((uint8_t *)dest_accessor.get() + dest_misalign,
					(uint8_t *)src_accessor.get() + src_misalign, chunk);

			node->_progress += chunk;
			return true;
		}

		static void fetchedDest(Worklet *base) {
			auto node = frg::container_of(base, &TransferNode::_worklet);
			if(!prepareSrcAndCopy(node))
				return;
			if(!process(node))
				return;

			WorkQueue::post(node->_copied);
		}

		static void fetchedSrc(Worklet *base) {
			auto node = frg::container_of(base, &TransferNode::_worklet);
			if(!doCopy(node))
				return;
			if(!process(node))
				return;

			WorkQueue::post(node->_copied);
		}
	};

	return Ops::process(node);
}

void Memory::copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t size) {
	(void)offset;
	(void)pointer;
	(void)size;
	frigg::panicLogger() << "Bundle does not support synchronous operations!" << frigg::endLog;
}

void Memory::resize(size_t new_length) {
	(void)new_length;
	frigg::panicLogger() << "Bundle does not support resize!" << frigg::endLog;
}


size_t Memory::getLength() {
	switch(tag()) {
	case MemoryTag::hardware: return static_cast<HardwareMemory *>(this)->getLength();
	case MemoryTag::allocated: return static_cast<AllocatedMemory *>(this)->getLength();
	case MemoryTag::backing: return static_cast<BackingMemory *>(this)->getLength();
	case MemoryTag::frontal: return static_cast<FrontalMemory *>(this)->getLength();
	default:
		frigg::panicLogger() << "Memory::getLength(): Unexpected tag" << frigg::endLog;
		__builtin_unreachable();
	}
}

void Memory::submitInitiateLoad(InitiateBase *initiate) {
	switch(tag()) {
	case MemoryTag::frontal:
		static_cast<FrontalMemory *>(this)->submitInitiateLoad(initiate);
		break;
	case MemoryTag::hardware:
	case MemoryTag::allocated:
		initiate->setup(kErrSuccess);
		initiate->complete();
		break;
	case MemoryTag::copyOnWrite:
		assert(!"Not implemented yet");
	default:
		assert(!"Not supported");
	}
}

void Memory::submitManage(ManageBase *handle) {
	switch(tag()) {
	case MemoryTag::backing:
		static_cast<BackingMemory *>(this)->submitManage(handle);
		break;
	default:
		assert(!"Not supported");
	}
}

void Memory::completeLoad(size_t offset, size_t length) {
	switch(tag()) {
	case MemoryTag::backing:
		static_cast<BackingMemory *>(this)->completeLoad(offset, length);
		break;
	default:
		assert(!"Not supported");
	}
}

// --------------------------------------------------------
// Copy operations.
// --------------------------------------------------------

void copyToBundle(Memory *bundle, ptrdiff_t offset, const void *pointer, size_t size,
		CopyToBundleNode *node, void (*complete)(CopyToBundleNode *)) {
	size_t progress = 0;
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, size);

		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!bundle->fetchRange(offset - misalign, &node->_fetch))
			assert(!"Handle the asynchronous case");
		
		auto page = node->_fetch.range().get<0>();
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy((uint8_t *)accessor.get() + misalign, pointer, prefix);
		progress += prefix;
	}

	while(size - progress >= kPageSize) {
		assert(!((offset + progress) % kPageSize));

		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!bundle->fetchRange(offset + progress, &node->_fetch))
			assert(!"Handle the asynchronous case");
		
		auto page = node->_fetch.range().get<0>();
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy(accessor.get(), (uint8_t *)pointer + progress, kPageSize);
		progress += kPageSize;
	}

	if(size - progress > 0) {
		assert(!((offset + progress) % kPageSize));
		
		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!bundle->fetchRange(offset + progress, &node->_fetch))
			assert(!"Handle the asynchronous case");
		
		auto page = node->_fetch.range().get<0>();
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{page};
		memcpy(accessor.get(), (uint8_t *)pointer + progress, size - progress);
	}

	complete(node);
}

void copyFromBundle(Memory *bundle, ptrdiff_t offset, void *buffer, size_t size,
		CopyFromBundleNode *node, void (*complete)(CopyFromBundleNode *)) {
	size_t progress = 0;
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, size);
		
		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!bundle->fetchRange(offset - misalign, &node->_fetch))
			assert(!"Handle the asynchronous case");

		auto page = node->_fetch.range().get<0>();
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy(buffer, (uint8_t *)accessor.get() + misalign, prefix);
		progress += prefix;
	}

	while(size - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);

		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!bundle->fetchRange(offset + progress, &node->_fetch))
			assert(!"Handle the asynchronous case");

		auto page = node->_fetch.range().get<0>();
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), kPageSize);
		progress += kPageSize;
	}

	if(size - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		
		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!bundle->fetchRange(offset + progress, &node->_fetch))
			assert(!"Handle the asynchronous case");
		
		auto page = node->_fetch.range().get<0>();
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), size - progress);
	}

	complete(node);
}

// --------------------------------------------------------
// HardwareMemory
// --------------------------------------------------------

HardwareMemory::HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode)
: Memory{MemoryTag::hardware}, _base{base}, _length{length}, _cacheMode{cache_mode} {
	assert(!(base % kPageSize));
	assert(!(length % kPageSize));
}

HardwareMemory::~HardwareMemory() {
	// For now we do nothing when deallocating hardware memory.
}

frigg::Tuple<PhysicalAddr, CachingMode> HardwareMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	return frigg::Tuple<PhysicalAddr, CachingMode>{_base + offset, _cacheMode};
}

bool HardwareMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	assert(offset % kPageSize == 0);

	completeFetch(node, _base + offset, _length - offset, _cacheMode);
	return true;
}

size_t HardwareMemory::getLength() {
	return _length;
}

// --------------------------------------------------------
// AllocatedMemory
// --------------------------------------------------------

AllocatedMemory::AllocatedMemory(size_t desired_length, size_t desired_chunk_size,
		size_t chunk_align)
: Memory(MemoryTag::allocated), _physicalChunks(*kernelAlloc), _chunkAlign(chunk_align) {
	static_assert(sizeof(unsigned long) == sizeof(uint64_t), "Fix use of __builtin_clzl");
	_chunkSize = size_t(1) << (64 - __builtin_clzl(desired_chunk_size - 1));
	if(_chunkSize != desired_chunk_size)
		frigg::infoLogger() << "\e[31mPhysical allocation of size " << (void *)desired_chunk_size
				<< " rounded up to power of 2\e[39m" << frigg::endLog;

	size_t length = (desired_length + (_chunkSize - 1)) & ~(_chunkSize - 1);
	if(length != desired_length)
		frigg::infoLogger() << "\e[31mMemory length " << (void *)desired_length
				<< " rounded up to chunk size " << (void *)_chunkSize
				<< "\e[39m" << frigg::endLog;

	assert(_chunkSize % kPageSize == 0);
	assert(_chunkAlign % kPageSize == 0);
	assert(_chunkSize % _chunkAlign == 0);
	_physicalChunks.resize(length / _chunkSize, PhysicalAddr(-1));
}

AllocatedMemory::~AllocatedMemory() {
	// TODO: This destructor takes a lock. This is potentially unexpected.
	// Rework this to only schedule the deallocation but not actually perform it?
	for(size_t i = 0; i < _physicalChunks.size(); ++i) {
		if(_physicalChunks[i] != PhysicalAddr(-1))
			physicalAllocator->free(_physicalChunks[i], _chunkSize);
	}
}

void AllocatedMemory::resize(size_t new_length) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(!(new_length % _chunkSize));
	size_t num_chunks = new_length / _chunkSize;
	assert(num_chunks >= _physicalChunks.size());
	_physicalChunks.resize(num_chunks, PhysicalAddr(-1));
}

void AllocatedMemory::copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	// TODO: For now we only allow naturally aligned access.
	assert(size <= kPageSize);
	assert(!(offset % size));

	size_t index = offset / _chunkSize;
	assert(index < _physicalChunks.size());
	if(_physicalChunks[index] == PhysicalAddr(-1)) {
		auto physical = physicalAllocator->allocate(_chunkSize);
		assert(physical != PhysicalAddr(-1));
		assert(!(physical % _chunkAlign));

		for(size_t pg_progress = 0; pg_progress < _chunkSize; pg_progress += kPageSize) {
			PageAccessor accessor{physical + pg_progress};
			memset(accessor.get(), 0, kPageSize);
		}
		_physicalChunks[index] = physical;
	}

	PageAccessor accessor{_physicalChunks[index]
			+ ((offset % _chunkSize) / kPageSize)};
	memcpy((uint8_t *)accessor.get() + (offset % kPageSize), pointer, size);
}

frigg::Tuple<PhysicalAddr, CachingMode> AllocatedMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	auto index = offset / _chunkSize;
	auto disp = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1))
		return frigg::Tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frigg::Tuple<PhysicalAddr, CachingMode>{_physicalChunks[index] + disp,
			CachingMode::null};
}

bool AllocatedMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	
	auto index = offset / _chunkSize;
	auto disp = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1)) {
		auto physical = physicalAllocator->allocate(_chunkSize);
		assert(physical != PhysicalAddr(-1));
		assert(!(physical & (_chunkAlign - 1)));

		for(size_t pg_progress = 0; pg_progress < _chunkSize; pg_progress += kPageSize) {
			PageAccessor accessor{physical + pg_progress};
			memset(accessor.get(), 0, kPageSize);
		}
		_physicalChunks[index] = physical;
	}

	assert(_physicalChunks[index] != PhysicalAddr(-1));
	completeFetch(node, _physicalChunks[index] + disp, _chunkSize - disp, CachingMode::null);
	return true;
}

size_t AllocatedMemory::getLength() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	return _physicalChunks.size() * _chunkSize;
}

// --------------------------------------------------------
// ManagedSpace
// --------------------------------------------------------

ManagedSpace::ManagedSpace(size_t length)
: physicalPages(*kernelAlloc), loadState(*kernelAlloc) {
	assert(length % kPageSize == 0);
	physicalPages.resize(length / kPageSize, PhysicalAddr(-1));
	loadState.resize(length / kPageSize, kStateMissing);
}

ManagedSpace::~ManagedSpace() {
	assert(!"Implement this");
}

// TODO: Split this into a function to match initiate <-> handle requests
// + a different function to complete initiate requests.
void ManagedSpace::progressLoads() {
	// TODO: Ensure that offset and length are actually in range.
	while(!initiateLoadQueue.empty()) {
		auto initiate = initiateLoadQueue.front();

		size_t index = (initiate->offset + initiate->progress) >> kPageShift;
		if(loadState[index] == kStateMissing) {
			if(submittedManageQueue.empty())
				break;

			// Fuse the request using adjacent missing pages.
			size_t count = 0;
			while(initiate->progress + (count << kPageShift) < initiate->length
					&& loadState[index] == kStateMissing) {
				loadState[index] = kStateLoading;
				index++;
				count++;
			}

			auto handle = submittedManageQueue.pop_front();
			handle->setup(kErrSuccess, initiate->offset + initiate->progress,
					count << kPageShift);
			completedManageQueue.push_back(handle);

			initiate->progress += count << kPageShift;
		}else if(loadState[index] == kStateLoading) {
			initiate->progress += kPageSize;
		}else{
			assert(loadState[index] == kStateLoaded);
			initiate->progress += kPageSize;
		}

		assert(initiate->progress <= initiate->length);
		if(initiate->progress == initiate->length) {
			if(isComplete(initiate)) {
				initiateLoadQueue.pop_front();
				initiate->setup(kErrSuccess);
				completedLoadQueue.push_back(initiate);
			}else{
				initiateLoadQueue.pop_front();
				pendingLoadQueue.push_back(initiate);
			}
		}
	}
}

bool ManagedSpace::isComplete(InitiateBase *initiate) {
	for(size_t p = 0; p < initiate->length; p += kPageSize) {
		size_t index = (initiate->offset + p) / kPageSize;
		if(loadState[index] != kStateLoaded)
			return false;
	}
	return true;
}

// --------------------------------------------------------
// BackingMemory
// --------------------------------------------------------

frigg::Tuple<PhysicalAddr, CachingMode> BackingMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	return frigg::Tuple<PhysicalAddr, CachingMode>{_managed->physicalPages[index],
			CachingMode::null};
}

bool BackingMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);
	
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);
	assert(index < _managed->physicalPages.size());
	if(_managed->physicalPages[index] == PhysicalAddr(-1)) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != PhysicalAddr(-1));
		
		PageAccessor accessor{physical};
		memset(accessor.get(), 0, kPageSize);
		_managed->physicalPages[index] = physical;
	}

	completeFetch(node, _managed->physicalPages[index] + misalign, kPageSize - misalign,
			CachingMode::null);
	return true;
}

size_t BackingMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->physicalPages.size() * kPageSize;
}

void BackingMemory::submitManage(ManageBase *handle) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	_managed->submittedManageQueue.push_back(handle);
	_managed->progressLoads();

	InitiateList initiate_queue;
	ManageList manage_queue;
	initiate_queue.splice(initiate_queue.end(), _managed->completedLoadQueue);
	manage_queue.splice(manage_queue.end(), _managed->completedManageQueue);

	lock.unlock();
	irq_lock.unlock();

	while(!initiate_queue.empty()) {
		auto node = initiate_queue.pop_front();
		node->complete();
	}
	while(!manage_queue.empty()) {
		auto node = manage_queue.pop_front();
		node->complete();
	}
}

void BackingMemory::completeLoad(size_t offset, size_t length) {
	assert((offset % kPageSize) == 0);
	assert((length % kPageSize) == 0);
	
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);
	assert((offset + length) / kPageSize <= _managed->physicalPages.size());

/*	assert(length == kPageSize);
	auto inspect = (unsigned char *)physicalToVirtual(_managed->physicalPages[offset / kPageSize]);
	auto log = frigg::infoLogger() << "dump";
	for(size_t b = 0; b < kPageSize; b += 16) {
		log << frigg::logHex(offset + b) << "   ";
		for(size_t i = 0; i < 16; i++)
			log << " " << frigg::logHex(inspect[b + i]);
		log << "\n";
	}
	log << frigg::endLog;*/

	for(size_t p = 0; p < length; p += kPageSize) {
		size_t index = (offset + p) / kPageSize;
		assert(_managed->loadState[index] == ManagedSpace::kStateLoading);
		_managed->loadState[index] = ManagedSpace::kStateLoaded;
	}

	InitiateList queue;
	for(auto it = _managed->pendingLoadQueue.begin(); it != _managed->pendingLoadQueue.end(); ) {
		auto it_copy = it;
		auto node = *it++;
		if(_managed->isComplete(node)) {
			_managed->pendingLoadQueue.erase(it_copy);
			queue.push_back(node);
		}
	}

	irq_lock.unlock();
	lock.unlock();

	while(!queue.empty()) {
		auto node = queue.pop_front();
		node->setup(kErrSuccess);
		node->complete();
	}
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

frigg::Tuple<PhysicalAddr, CachingMode> FrontalMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	if(_managed->loadState[index] != ManagedSpace::kStateLoaded)
		return frigg::Tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frigg::Tuple<PhysicalAddr, CachingMode>{_managed->physicalPages[index],
			CachingMode::null};
}

bool FrontalMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);
	assert(index < _managed->physicalPages.size());
	if(_managed->loadState[index] != ManagedSpace::kStateLoaded) {
		// TODO: Do not allocate memory here; use pre-allocated nodes instead.
		struct Closure {
			uintptr_t offset;
			FetchNode *fetch;
			ManagedSpace *bundle;

			Worklet worklet;
			InitiateBase initiate;
		} *closure = frigg::construct<Closure>(*kernelAlloc);

		struct Ops {
			static void initiated(Worklet *worklet) {
				auto closure = frg::container_of(worklet, &Closure::worklet);
				assert(closure->initiate.error() == kErrSuccess);

				auto irq_lock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&closure->bundle->mutex);

				auto index = closure->offset >> kPageShift;
				auto misalign = closure->offset & (kPageSize - 1);
				assert(closure->bundle->loadState[index] == ManagedSpace::kStateLoaded);
				auto physical = closure->bundle->physicalPages[index];
				assert(physical != PhysicalAddr(-1));

				lock.unlock();
				irq_lock.unlock();

				completeFetch(closure->fetch, physical + misalign, kPageSize - misalign,
						CachingMode::null);
				callbackFetch(closure->fetch);
				frigg::destruct(*kernelAlloc, closure);
			}
		};

		closure->offset = offset;
		closure->fetch = node;
		closure->bundle = _managed.get();

		closure->worklet.setup(&Ops::initiated);
		closure->initiate.setup(offset, kPageSize, &closure->worklet);
		_managed->initiateLoadQueue.push_back(&closure->initiate);
		_managed->progressLoads();

		ManageList manage_queue;
		manage_queue.splice(manage_queue.end(), _managed->completedManageQueue);
		assert(_managed->completedLoadQueue.empty());

		lock.unlock();
		irq_lock.unlock();

		while(!manage_queue.empty()) {
			auto node = manage_queue.pop_front();
			node->complete();
		}

		return false;
	}

	auto physical = _managed->physicalPages[index];
	assert(physical != PhysicalAddr(-1));
	completeFetch(node, physical + misalign, kPageSize - misalign, CachingMode::null);
	return true;
}

size_t FrontalMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->physicalPages.size() * kPageSize;
}

void FrontalMemory::submitInitiateLoad(InitiateBase *initiate) {
	initiate->progress = 0;

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	assert(initiate->offset % kPageSize == 0);
	assert(initiate->length % kPageSize == 0);
	assert((initiate->offset + initiate->length) / kPageSize
			<= _managed->physicalPages.size());
	
	_managed->initiateLoadQueue.push_back(initiate);
	_managed->progressLoads();

	InitiateList initiate_queue;
	ManageList manage_queue;
	initiate_queue.splice(initiate_queue.end(), _managed->completedLoadQueue);
	manage_queue.splice(manage_queue.end(), _managed->completedManageQueue);

	lock.unlock();
	irq_lock.unlock();

	while(!initiate_queue.empty()) {
		auto node = initiate_queue.pop_front();
		node->complete();
	}
	while(!manage_queue.empty()) {
		auto node = manage_queue.pop_front();
		node->complete();
	}
}

// --------------------------------------------------------

ExteriorBundleView::ExteriorBundleView(frigg::SharedPtr<MemoryBundle> bundle,
		ptrdiff_t view_offset, size_t view_size)
: _bundle{frigg:move(bundle)}, _viewOffset{view_offset}, _viewSize{view_size} {
	assert(!(_viewOffset & (kPageSize - 1)));
	assert(!(_viewSize & (kPageSize - 1)));
}

size_t ExteriorBundleView::length() {
	return _viewSize;
}

ViewRange ExteriorBundleView::translateRange(ptrdiff_t offset, size_t size) {
	assert(offset + size <= _viewSize);
	return ViewRange{_bundle.get(), _viewOffset + offset,
			frigg::min(size, _viewSize - offset), ViewRestriction::null};
}

// --------------------------------------------------------
// HoleAggregator
// --------------------------------------------------------

bool HoleAggregator::aggregate(Hole *hole) {
	size_t size = hole->length();
	if(HoleTree::get_left(hole) && HoleTree::get_left(hole)->largestHole > size)
		size = HoleTree::get_left(hole)->largestHole;
	if(HoleTree::get_right(hole) && HoleTree::get_right(hole)->largestHole > size)
		size = HoleTree::get_right(hole)->largestHole;
	
	if(hole->largestHole == size)
		return false;
	hole->largestHole = size;
	return true;
}

bool HoleAggregator::check_invariant(HoleTree &tree, Hole *hole) {
	auto pred = tree.predecessor(hole);
	auto succ = tree.successor(hole);

	// Check largest hole invariant.
	size_t size = hole->length();
	if(tree.get_left(hole) && tree.get_left(hole)->largestHole > size)
		size = tree.get_left(hole)->largestHole;
	if(tree.get_right(hole) && tree.get_right(hole)->largestHole > size)
		size = tree.get_right(hole)->largestHole;
	
	if(hole->largestHole != size) {
		frigg::infoLogger() << "largestHole violation: " << "Expected " << size
				<< ", got " << hole->largestHole << "." << frigg::endLog;
		return false;
	}

	// Check non-overlapping memory areas invariant.
	if(pred && hole->address() < pred->address() + pred->length()) {
		frigg::infoLogger() << "Non-overlapping (left) violation" << frigg::endLog;
		return false;
	}
	if(succ && hole->address() + hole->length() > succ->address()) {
		frigg::infoLogger() << "Non-overlapping (right) violation" << frigg::endLog;
		return false;
	}
	
	return true;
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(AddressSpace *owner, VirtualAddr base_address, size_t length,
		MappingFlags flags)
: _owner{owner}, _address{base_address}, _length{length}, _flags{flags} { }

// --------------------------------------------------------
// NormalMapping
// --------------------------------------------------------

NormalMapping::NormalMapping(AddressSpace *owner, VirtualAddr address, size_t length,
		MappingFlags flags, frigg::SharedPtr<VirtualView> view, uintptr_t offset)
: Mapping{owner, address, length, flags}, _view{frigg::move(view)}, _offset{offset} {
	assert(_offset + NormalMapping::length() <= _view->length());
}

frigg::Tuple<PhysicalAddr, CachingMode>
NormalMapping::resolveRange(ptrdiff_t offset) {
	// TODO: This function should be rewritten.
	assert((size_t)offset + kPageSize <= length());
	auto range = _view->translateRange(_offset + offset,
			frigg::min((size_t)kPageSize, length() - (size_t)offset));
	auto bundle_range = range.bundle->peekRange(range.displacement);
	return frigg::Tuple<PhysicalAddr, CachingMode>{bundle_range.get<0>(), bundle_range.get<1>()};
}

bool NormalMapping::prepareRange(PrepareNode *node) {
	struct Closure {
		NormalMapping *self;
		PrepareNode *node;
		size_t progress;
		Worklet worklet;
		FetchNode fetch;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	closure->self = this;
	closure->node = node;
	closure->progress = 0;

	struct Ops {
		static bool process(Closure *closure) {
			while(closure->progress < closure->node->_size)
				if(!transfer(closure))
					return false;
			return true;
		}
		
		static bool transfer(Closure *closure) {
			// TODO: Assert that there is no overflow.
			auto self = closure->self;
			auto offset = closure->node->_offset + closure->progress;
			
			auto view_range = self->_view->translateRange(self->_offset + offset,
					closure->node->_size - offset);
			closure->worklet.setup(&Ops::fetched);
			closure->fetch.setup(&closure->worklet);
			if(!view_range.bundle->fetchRange(view_range.displacement, &closure->fetch))
				return false;

			closure->progress += closure->fetch.range().get<1>();
			return true;
		}
		
		static void fetched(Worklet *base) {
			auto closure = frg::container_of(base, &Closure::worklet);
			if(!process(closure))
				return;

			WorkQueue::post(closure->node->_prepared);
			frigg::destruct(*kernelAlloc, closure);
		}

	};

	if(!Ops::process(closure))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

Mapping *NormalMapping::shareMapping(AddressSpace *dest_space) {
	// TODO: Always keep the exact flags?
	return frigg::construct<NormalMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), _view, _offset);
}

Mapping *NormalMapping::copyOnWrite(AddressSpace *dest_space) {
	auto chain = frigg::makeShared<CowChain>(*kernelAlloc, _view, _offset, length());
	return frigg::construct<CowMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), frigg::move(chain));
}

void NormalMapping::install(bool overwrite) {
	uint32_t page_flags = 0;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
		page_flags |= page_access::write;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
		page_flags |= page_access::execute;
	// TODO: Allow inaccessible mappings.
	assert((flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		// TODO: Add a don't-require-backing flag to peekRange.
		//if(flags() & MappingFlags::dontRequireBacking)
		//	grab_flags |= kGrabDontRequireBacking;

		auto range = _view->translateRange(_offset + progress, kPageSize);
		assert(range.size >= kPageSize);
		auto bundle_range = range.bundle->peekRange(range.displacement);

		VirtualAddr vaddr = address() + progress;
		if(overwrite && owner()->_pageSpace.isMapped(vaddr)) {
			owner()->_pageSpace.unmapRange(vaddr, kPageSize, PageMode::normal);
		}else{
			assert(!owner()->_pageSpace.isMapped(vaddr));
		}
		if(bundle_range.get<0>() != PhysicalAddr(-1))
			owner()->_pageSpace.mapSingle4k(vaddr, bundle_range.get<0>(), true,
					page_flags, bundle_range.get<1>());
	}
}

void NormalMapping::uninstall(bool clear) {
	if(!clear)
		return;

	owner()->_pageSpace.unmapRange(address(), length(), PageMode::remap);
}

// --------------------------------------------------------
// CowMapping
// --------------------------------------------------------

CowChain::CowChain(frigg::SharedPtr<VirtualView> view, ptrdiff_t offset, size_t size)
: _superRoot{frigg::move(view)}, _superOffset{offset}, _pages{kernelAlloc.get()} {
	assert(!(size & (kPageSize - 1)));
	_copy = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, kPageSize, kPageSize);
}

CowChain::CowChain(frigg::SharedPtr<CowChain> chain, ptrdiff_t offset, size_t size)
: _superChain{frigg::move(chain)}, _superOffset{offset}, _pages{kernelAlloc.get()} {
	assert(!(size & (kPageSize - 1)));
	_copy = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, kPageSize, kPageSize);
}

// --------------------------------------------------------

CowMapping::CowMapping(AddressSpace *owner, VirtualAddr address, size_t length,
		MappingFlags flags, frigg::SharedPtr<CowChain> chain)
: Mapping{owner, address, length, flags}, _chain{frigg::move(chain)} {
}

frigg::Tuple<PhysicalAddr, CachingMode>
CowMapping::resolveRange(ptrdiff_t offset) {
	if(auto it = _chain->_pages.find(offset >> kPageShift); it) {
		auto physical = it->load(std::memory_order_relaxed);
		return frigg::Tuple<PhysicalAddr, CachingMode>{physical, CachingMode::null};
	}

	return frigg::Tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
}

bool CowMapping::prepareRange(PrepareNode *node) {
	struct Closure {
		CowMapping *self;
		PrepareNode *node;
		size_t progress;
		Worklet worklet;
		TransferNode copy;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	closure->self = this;
	closure->node = node;
	closure->progress = 0;

	struct Ops {
		static bool process(Closure *closure) {
			while(closure->progress < closure->node->_size)
				if(!transfer(closure))
					return false;
			return true;
		}
		
		static bool transfer(Closure *closure) {
			// TODO: Assert that there is no overflow.
			auto self = closure->self;
			auto offset = closure->node->_offset + closure->progress;
			auto page = offset & ~(kPageSize - 1);

			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_chain->_mutex);

			// If the page is present in this bundle we just return it.
			if(auto it = self->_chain->_pages.find(page >> kPageShift); it) {
				auto physical = it->load(std::memory_order_relaxed);
				assert(physical != PhysicalAddr(-1));
				closure->progress += kPageSize - (offset & (kPageSize - 1));
				return true;
			}

			// Otherwise we need to copy from the super-tree.
			auto source = self->_chain.get();
			ptrdiff_t disp = page;
			while(true) {
				// Copy from a descendant CoW bundle.
				if(auto it = source->_pages.find(disp >> kPageShift); it) {
					// Cannot copy from ourselves; this case is handled above.
					assert(source != self->_chain.get());
					closure->copy.setup(self->_chain->_copy.get(), page, source->_copy.get(),
							disp, kPageSize, nullptr);
					if(!Memory::transfer(&closure->copy))
						assert(!"Fix the asynchronous case");

					auto range = self->_chain->_copy->peekRange(page);
					auto physical = range.get<0>();
					assert(physical != PhysicalAddr(-1));
					auto cow_it = self->_chain->_pages.insert(page >> kPageShift,
							PhysicalAddr(-1));
					cow_it->store(physical, std::memory_order_relaxed);
					closure->progress += kPageSize - (offset & (kPageSize - 1));
					return true;
				}

				// Copy from the root view.
				if(!source->_superChain) {
					assert(source->_superRoot);
					auto view_range = source->_superRoot->translateRange(source->_superOffset + disp,
							kPageSize);
					assert(view_range.size >= kPageSize);

					closure->worklet.setup(&Ops::copied);
					closure->copy.setup(self->_chain->_copy.get(), page, view_range.bundle,
							view_range.displacement, kPageSize, &closure->worklet);
					if(!Memory::transfer(&closure->copy))
						return false;

					finish(closure);
					return true;
				}

				disp += source->_superOffset;
				source = source->_superChain.get();
			}
		}

		static void finish(Closure *closure) {
			// TODO: Assert that there is no overflow.
			auto self = closure->self;
			auto offset = closure->node->_offset + closure->progress;
			auto page = offset & ~(kPageSize - 1);

			auto bundle_range = self->_chain->_copy->peekRange(page);
			auto physical = bundle_range.get<0>();
			assert(physical != PhysicalAddr(-1));
			auto cow_it = self->_chain->_pages.insert(page >> kPageShift,
					PhysicalAddr(-1));
			cow_it->store(physical, std::memory_order_relaxed);
			closure->progress += kPageSize - (offset & (kPageSize - 1));
		}

		static void copied(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &Closure::worklet);
			finish(closure);

			WorkQueue::post(closure->node->_prepared);
			frigg::destruct(*kernelAlloc, closure);
		}
	};

	if(!Ops::process(closure))
		return false;
	
	frigg::destruct(*kernelAlloc, closure);
	return true;
}

Mapping *CowMapping::shareMapping(AddressSpace *dest_space) {
	(void)dest_space;
	assert(!"Fix this");
	__builtin_unreachable();
}

Mapping *CowMapping::copyOnWrite(AddressSpace *dest_space) {
	auto sub_chain = frigg::makeShared<CowChain>(*kernelAlloc, _chain, 0, length());
	return frigg::construct<CowMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), frigg::move(sub_chain));
}

void CowMapping::install(bool overwrite) {
	// For now we just unmap everything. TODO: Map available pages.
	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		VirtualAddr vaddr = address() + progress;
		if(overwrite && owner()->_pageSpace.isMapped(vaddr)) {
			owner()->_pageSpace.unmapRange(vaddr, kPageSize, PageMode::normal);
		}else{
			assert(!owner()->_pageSpace.isMapped(vaddr));
		}
	}
}

void CowMapping::uninstall(bool clear) {
	if(!clear)
		return;

	owner()->_pageSpace.unmapRange(address(), length(), PageMode::remap);
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------



// --------------------------------------------------------

AddressSpace::AddressSpace() { }

AddressSpace::~AddressSpace() {
	Hole *hole = _holes.get_root();
	while(hole) {
		auto next = HoleTree::successor(hole);
		_holes.remove(hole);
		frg::destruct(*kernelAlloc, hole);
		hole = next;
	}

	Mapping *mapping = _mappings.get_root();
	while(mapping) {
		auto next = MappingTree::successor(mapping);
		_mappings.remove(mapping);
		frg::destruct(*kernelAlloc, mapping);
		mapping = next;
	}
}

void AddressSpace::setupDefaultMappings() {
	auto hole = frigg::construct<Hole>(*kernelAlloc, 0x100000, 0x7ffffff00000);
	_holes.insert(hole);
}

Error AddressSpace::map(Guard &guard,
		frigg::UnsafePtr<VirtualView> view, VirtualAddr address,
		size_t offset, size_t length, uint32_t flags, VirtualAddr *actual_address) {
	assert(guard.protects(&lock));
	assert(length);
	assert(!(length % kPageSize));

	if(offset + length > view->length())
		return kErrBufferTooSmall;

	VirtualAddr target;
	if(flags & kMapFixed) {
		assert(address);
		assert((address % kPageSize) == 0);
		target = _allocateAt(address, length);
	}else{
		target = _allocate(length, flags);
	}
	assert(target);

//	frigg::infoLogger() << "Creating new mapping at " << (void *)target
//			<< ", length: " << (void *)length << frigg::endLog;
	
	// Setup a new Mapping object.
	std::underlying_type_t<MappingFlags> mapping_flags = 0;
	
	// TODO: This is a hack to "upgrade" CoW@fork to CoW.
	// TODO: Remove this once we remove CopyOnWriteAtFork.
	if(flags & kMapCopyOnWriteAtFork)
		flags |= kMapCopyOnWrite;
	if((flags & kMapCopyOnWrite) || (flags & kMapCopyOnWriteAtFork))
		mapping_flags |= MappingFlags::copyOnWriteAtFork;
	
	if(flags & kMapDropAtFork) {
		mapping_flags |= MappingFlags::dropAtFork;
	}else if(flags & kMapShareAtFork) {
		mapping_flags |= MappingFlags::shareAtFork;
	}

	// TODO: The upgrading mechanism needs to be arch-specific:
	// Some archs might only support RX, while other support X.
	auto mask = kMapProtRead | kMapProtWrite | kMapProtExecute;
	if((flags & mask) == (kMapProtRead | kMapProtWrite | kMapProtExecute)
			|| (flags & mask) == (kMapProtWrite | kMapProtExecute)) {
		// WX is upgraded to RWX.
		mapping_flags |= MappingFlags::protRead | MappingFlags::protWrite
			| MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtExecute)
			|| (flags & mask) == kMapProtExecute) {
		// X is upgraded to RX.
		mapping_flags |= MappingFlags::protRead | MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtWrite)
			|| (flags & mask) == kMapProtWrite) {
		// W is upgraded to RW.
		mapping_flags |= MappingFlags::protRead | MappingFlags::protWrite;
	}else if((flags & mask) == kMapProtRead) {
		mapping_flags |= MappingFlags::protRead;
	}else{
		assert(!(flags & mask));
	}
	
	if(flags & kMapDontRequireBacking)
		mapping_flags |= MappingFlags::dontRequireBacking;

	Mapping *mapping;
	if(flags & kMapCopyOnWrite) {
		auto chain = frigg::makeShared<CowChain>(*kernelAlloc, view.toShared(), offset, length);
		mapping = frigg::construct<CowMapping>(*kernelAlloc, this, target, length,
				static_cast<MappingFlags>(mapping_flags), frigg::move(chain));
	}else{
		mapping = frigg::construct<NormalMapping>(*kernelAlloc, this, target, length,
				static_cast<MappingFlags>(mapping_flags), view.toShared(), offset);
	}

	// Install the new mapping object.
	_mappings.insert(mapping);
	assert(!(flags & kMapPopulate));
	mapping->install(false);

	*actual_address = target;
	return kErrSuccess;
}

void AddressSpace::unmap(Guard &guard, VirtualAddr address, size_t length,
		AddressUnmapNode *node) {
	assert(guard.protects(&lock));

	Mapping *mapping = _getMapping(address);
	assert(mapping);

	// TODO: Allow shrinking of the mapping.
	assert(mapping->address() == address);
	assert(mapping->length() == length);
	mapping->uninstall(true);

	_mappings.remove(mapping);
	frigg::destruct(*kernelAlloc, mapping);

	node->_shootNode.shotDown = [] (ShootNode *sn) {
		auto node = frg::container_of(sn, &AddressUnmapNode::_shootNode);

		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&node->_space->lock);

		// Find the holes that preceede/succeede mapping.
		Hole *pre;
		Hole *succ;

		auto current = node->_space->_holes.get_root();
		while(true) {
			assert(current);
			if(sn->address < current->address()) {
				if(HoleTree::get_left(current)) {
					current = HoleTree::get_left(current);
				}else{
					pre = HoleTree::predecessor(current);
					succ = current;
					break;
				}
			}else{
				assert(sn->address >= current->address() + current->length());
				if(HoleTree::get_right(current)) {
					current = HoleTree::get_right(current);
				}else{
					pre = current;
					succ = HoleTree::successor(current);
					break;
				}
			}
		}

		// Try to merge the new hole and the existing ones.
		if(pre && pre->address() + pre->length() == sn->address
				&& succ && sn->address + sn->size == succ->address()) {
			auto hole = frigg::construct<Hole>(*kernelAlloc, pre->address(),
					pre->length() + sn->size + succ->length());

			node->_space->_holes.remove(pre);
			node->_space->_holes.remove(succ);
			node->_space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, pre);
			frigg::destruct(*kernelAlloc, succ);
		}else if(pre && pre->address() + pre->length() == sn->address) {
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					pre->address(), pre->length() + sn->size);

			node->_space->_holes.remove(pre);
			node->_space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, pre);
		}else if(succ && sn->address + sn->size == succ->address()) {
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					sn->address, sn->size + succ->length());

			node->_space->_holes.remove(succ);
			node->_space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, succ);
		}else{
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					sn->address, sn->size);

			node->_space->_holes.insert(hole);
		}
	};

	node->_space = this;
	node->_shootNode.address = address;
	node->_shootNode.size = length;

	// Work around a deadlock if submitShootdown() invokes shotDown() immediately.
	// TODO: This should probably be resolved by running shotDown() from some callback queue.
	guard.unlock();

	_pageSpace.submitShootdown(&node->_shootNode);
}

bool AddressSpace::handleFault(VirtualAddr address, uint32_t fault_flags, FaultNode *node) {
	node->_address = address;
	node->_flags = fault_flags;

	Mapping *mapping;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&lock);

		mapping = _getMapping(address);
		if(!mapping) {
			node->_resolved = false;
			return true;
		}
	}
	
	// FIXME: mapping might be deleted here!
	// We need to use either refcounting or QS garbage collection here!
	
	node->_mapping = mapping;

	// Here we do the mapping-based fault handling.
	if(node->_flags & AddressSpace::kFaultWrite)
		if(!((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)) {
			node->_resolved = false;
			return true;
		}
	if(node->_flags & AddressSpace::kFaultExecute)
		if(!((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)) {
			node->_resolved = false;
			return true;
		}

	struct Ops {
		static void prepared(Worklet *base) {
			auto node = frg::container_of(base, &FaultNode::_worklet);
			update(node);
			WorkQueue::post(node->_handled);
		}
		
		static void update(FaultNode *node) {
			auto mapping = node->_mapping;

			auto fault_page = (node->_address - mapping->address()) & ~(kPageSize - 1);
			auto vaddr = mapping->address() + fault_page;
			// TODO: This can actually happen!
			assert(!mapping->owner()->_pageSpace.isMapped(vaddr));

			uint32_t page_flags = 0;
			if((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
				page_flags |= page_access::write;
			if((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
				page_flags |= page_access::execute;
			// TODO: Allow inaccessible mappings.
			assert((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

			auto range = mapping->resolveRange(fault_page);
			assert(range.get<0>() != PhysicalAddr(-1));

			mapping->owner()->_pageSpace.mapSingle4k(vaddr, range.get<0>(),
					true, page_flags, range.get<1>());
			node->_resolved = true;
		}
	};

	auto fault_page = (node->_address - mapping->address()) & ~(kPageSize - 1);
	node->_worklet.setup(Ops::prepared);
	node->_prepare.setup(fault_page, kPageSize, &node->_worklet);
	if(mapping->prepareRange(&node->_prepare)) {
		Ops::update(node);
		return true;
	}else{
		return false;
	}
}

bool AddressSpace::fork(ForkNode *node) {
	node->_fork = frigg::makeShared<AddressSpace>(*kernelAlloc);
	node->_original = this;

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&this->lock);

	// Copy holes to the child space.
	auto cur_hole = _holes.first();
	while(cur_hole) {
		auto fork_hole = frigg::construct<Hole>(*kernelAlloc,
				cur_hole->address(), cur_hole->length());
		node->_fork->_holes.insert(fork_hole);

		cur_hole = HoleTree::successor(cur_hole);
	}

	// Modify memory mapping of both spaces.
	auto cur_mapping = _mappings.first();
	while(cur_mapping) {
		auto successor = MappingTree::successor(cur_mapping);

		if(cur_mapping->flags() & MappingFlags::dropAtFork) {
			// TODO: Merge this hole into adjacent holes.
			auto fork_hole = frigg::construct<Hole>(*kernelAlloc,
					cur_mapping->address(), cur_mapping->length());
			node->_fork->_holes.insert(fork_hole);
		}else if(cur_mapping->flags() & MappingFlags::shareAtFork) {
			auto fork_mapping = cur_mapping->shareMapping(node->_fork.get());

			node->_fork->_mappings.insert(fork_mapping);
			fork_mapping->install(false);
		}else if(cur_mapping->flags() & MappingFlags::copyOnWriteAtFork) {
			// TODO: Copy-on-write if possible and plain copy otherwise.
			// TODO: Decide if we want a copy-on-write or a real copy of the mapping.
			// * Pinned mappings prevent CoW.
			//     This is necessary because CoW may change mapped pages
			//     in the original space.
			// * Futexes attached to the memory object prevent CoW.
			//     This ensures that processes do not miss wake ups in the original space.
			if(false) {
				auto origin_mapping = cur_mapping->copyOnWrite(this);
				auto fork_mapping = cur_mapping->copyOnWrite(node->_fork.get());

				_mappings.remove(cur_mapping);
				_mappings.insert(origin_mapping);
				node->_fork->_mappings.insert(fork_mapping);
				cur_mapping->uninstall(false);
				origin_mapping->install(true);
				fork_mapping->install(false);
				frigg::destruct(*kernelAlloc, cur_mapping);
			}else{
				auto bundle = frigg::makeShared<AllocatedMemory>(*kernelAlloc,
						cur_mapping->length(), kPageSize, kPageSize);

				node->_items.addBack(ForkItem{cur_mapping, bundle.get()});

				auto view = frigg::makeShared<ExteriorBundleView>(*kernelAlloc,
						frigg::move(bundle), 0, cur_mapping->length());

				auto fork_mapping = frigg::construct<NormalMapping>(*kernelAlloc,
						node->_fork.get(), cur_mapping->address(), cur_mapping->length(),
						cur_mapping->flags(), frigg::move(view), 0);
				node->_fork->_mappings.insert(fork_mapping);
				fork_mapping->install(false);
			}
		}else{
			assert(!"Illegal mapping type");
		}

		cur_mapping = successor;
	}

	// TODO: Unlocking here should not be necessary.
	lock.unlock();
	irq_lock.unlock();

	node->_progress = 0;

	struct Ops {
		static bool process(ForkNode *node) {
			while(!node->_items.empty()) {
				auto item = &node->_items.front();
				if(node->_progress == item->mapping->length()) {
					node->_items.removeFront();
					node->_progress = 0;
					continue;
				}
				assert(node->_progress < item->mapping->length());
				assert(node->_progress + kPageSize <= item->mapping->length());

				node->_worklet.setup(&Ops::prepared);
				node->_prepare.setup(node->_progress, kPageSize, &node->_worklet);
				if(!item->mapping->prepareRange(&node->_prepare))
					return false;
				doCopy(node);
			}

			return true;
		}

		static void prepared(Worklet *base) {
			auto node = frg::container_of(base, &ForkNode::_worklet);
			doCopy(node);
			if(!process(node))
				return;

			WorkQueue::post(node->_forked);
		}

		static void doCopy(ForkNode *node) {
			auto item = &node->_items.front();
			auto range = item->mapping->resolveRange(node->_progress);
			assert(range.get<0>() != PhysicalAddr(-1));

			PageAccessor accessor{range.get<0>()};
			item->destBundle->copyKernelToThisSync(node->_progress, accessor.get(), kPageSize);
			node->_progress += kPageSize;
		}
	};

	return Ops::process(node);
}

void AddressSpace::activate() {
	_pageSpace.activate();
}

Mapping *AddressSpace::_getMapping(VirtualAddr address) {
	auto current = _mappings.get_root();
	while(current) {
		if(address < current->address()) {
			current = MappingTree::get_left(current);
		}else if(address >= current->address() + current->length()) {
			current = MappingTree::get_right(current);
		}else{
			assert(address >= current->address()
					&& address < current->address() + current->length());
			return current;
		}
	}

	return nullptr;
}

VirtualAddr AddressSpace::_allocate(size_t length, MapFlags flags) {
	assert(length > 0);
	assert((length % kPageSize) == 0);
//	frigg::infoLogger() << "Allocate virtual memory area"
//			<< ", size: 0x" << frigg::logHex(length) << frigg::endLog;

	if(_holes.get_root()->largestHole < length)
		return 0; // TODO: Return something else here?
	
	auto current = _holes.get_root();
	while(true) {
		if(flags & kMapPreferBottom) {
			// Try to allocate memory at the bottom of the range.
			if(HoleTree::get_left(current)
					&& HoleTree::get_left(current)->largestHole >= length) {
				current = HoleTree::get_left(current);
				continue;
			}
			
			if(current->length() >= length) {
				_splitHole(current, 0, length);
				return current->address();
			}

			assert(HoleTree::get_right(current));
			assert(HoleTree::get_right(current)->largestHole >= length);
			current = HoleTree::get_right(current);
		}else{
			// Try to allocate memory at the top of the range.
			assert(flags & kMapPreferTop);
			
			if(HoleTree::get_right(current)
					&& HoleTree::get_right(current)->largestHole >= length) {
				current = HoleTree::get_right(current);
				continue;
			}

			if(current->length() >= length) {
				size_t offset = current->length() - length;
				_splitHole(current, offset, length);
				return current->address() + offset;
			}

			assert(HoleTree::get_left(current));
			assert(HoleTree::get_left(current)->largestHole >= length);
			current = HoleTree::get_left(current);
		}
	}
}

VirtualAddr AddressSpace::_allocateAt(VirtualAddr address, size_t length) {
	assert(!(address % kPageSize));
	assert(!(length % kPageSize));

	auto current = _holes.get_root();
	while(true) {
		// TODO: Otherwise, this method fails.
		assert(current);

		if(address < current->address()) {
			current = HoleTree::get_left(current);
		}else if(address >= current->address() + current->length()) {
			current = HoleTree::get_right(current);
		}else{
			assert(address >= current->address()
					&& address < current->address() + current->length());
			break;
		}
	}
	
	_splitHole(current, address - current->address(), length);
	return address;
}

void AddressSpace::_splitHole(Hole *hole, VirtualAddr offset, size_t length) {
	assert(length);
	assert(offset + length <= hole->length());
	
	_holes.remove(hole);

	if(offset) {
		auto predecessor = frigg::construct<Hole>(*kernelAlloc, hole->address(), offset);
		_holes.insert(predecessor);
	}

	if(offset + length < hole->length()) {
		auto successor = frigg::construct<Hole>(*kernelAlloc,
				hole->address() + offset + length, hole->length() - (offset + length));
		_holes.insert(successor);
	}
	
	frigg::destruct(*kernelAlloc, hole);
}

// --------------------------------------------------------
// ForeignSpaceAccessor
// --------------------------------------------------------

bool ForeignSpaceAccessor::acquire(AcquireNode *node) {
	node->_accessor = this;

	if(!node->_accessor->_length) {
		node->_accessor->_acquired = true;
		return true;
	}

	struct Ops {
		static void prepared(Worklet *base) {
			assert(!"This is untested");
			auto node = frg::container_of(base, &AcquireNode::_worklet);
			node->_accessor->_acquired = true;
			WorkQueue::post(node->_acquired);
		}
	};

	// TODO: Verify the mapping's size.
	auto vaddr = reinterpret_cast<uintptr_t>(node->_accessor->_address);
	if(!vaddr)
		return false; // TODO: Remove this debugging test.
	auto mapping = node->_accessor->_space->_getMapping(vaddr);
	assert(mapping);
	node->_worklet.setup(&Ops::prepared);
	node->_prepare.setup(vaddr - mapping->address(), node->_accessor->_length, &node->_worklet);
	if(mapping->prepareRange(&node->_prepare)) {
		node->_accessor->_acquired = true;
		return true;
	}
	return false;
}

PhysicalAddr ForeignSpaceAccessor::getPhysical(size_t offset) {
	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard guard(&_space->lock);

	auto vaddr = reinterpret_cast<VirtualAddr>(_address) + offset;
	return _resolvePhysical(vaddr);
}

void ForeignSpaceAccessor::load(size_t offset, void *pointer, size_t size) {
	assert(_acquired);

	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard guard(&_space->lock);
	
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _resolvePhysical(write - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy((char *)pointer + progress, (char *)accessor.get() + misalign, chunk);
		progress += chunk;
	}
}

Error ForeignSpaceAccessor::write(size_t offset, const void *pointer, size_t size) {
	assert(_acquired);

	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard guard(&_space->lock);
	
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _resolvePhysical(write - misalign);
		if(page == PhysicalAddr(-1))
			return kErrFault;

		PageAccessor accessor{page};
		memcpy((char *)accessor.get() + misalign, (char *)pointer + progress, chunk);
		progress += chunk;
	}

	return kErrSuccess;
}

PhysicalAddr ForeignSpaceAccessor::_resolvePhysical(VirtualAddr vaddr) {
	Mapping *mapping = _space->_getMapping(vaddr);
	assert(mapping);
	auto range = mapping->resolveRange(vaddr - mapping->address());
	return range.get<0>();
}

} // namespace thor

