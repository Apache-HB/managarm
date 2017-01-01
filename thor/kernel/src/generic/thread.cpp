
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

void Thread::deferCurrent() {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;

	assert(!intsAreEnabled());
	if(forkExecutor()) {
		ScheduleGuard schedule_lock(scheduleLock.get());
		enqueueInSchedule(schedule_lock, this_thread);
		
		runDetached([] (ScheduleGuard schedule_lock) {
			doSchedule(frigg::move(schedule_lock));
		}, frigg::move(schedule_lock));
	}
}

void Thread::blockCurrent(void *, void (*) (void *)) {
	assert(!"Use blockCurrentWhile() instead");
}

void Thread::interruptCurrent(Interrupt interrupt, FaultImageAccessor image) {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunInterrupted;
	saveExecutor(image);

	while(!this_thread->_observeQueue.empty()) {
		auto observe = this_thread->_observeQueue.removeFront();
		observe->trigger(Error::kErrSuccess, interrupt);
	}

	assert(!intsAreEnabled());
	runDetached([] {
		ScheduleGuard schedule_lock(scheduleLock.get());
		doSchedule(frigg::move(schedule_lock));
	});
}

void Thread::interruptCurrent(Interrupt interrupt, SyscallImageAccessor image) {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunInterrupted;
	saveExecutor(image);

	// FIXME: Huge hack! This should really be a loop and run more than one callback.
	if(!this_thread->_observeQueue.empty()) {
		auto observe = this_thread->_observeQueue.removeFront();
		assert(this_thread->_observeQueue.empty());
		observe->trigger(Error::kErrSuccess, interrupt);
	}

	assert(!intsAreEnabled());
	runDetached([] {
		ScheduleGuard schedule_lock(scheduleLock.get());
		doSchedule(frigg::move(schedule_lock));
	});
}

void Thread::raiseSignals(SyscallImageAccessor image) {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	
	if(this_thread->_pendingSignal == kSigStop) {
		this_thread->_runState = kRunInterrupted;
		saveExecutor(image);

		// FIXME: Huge hack! This should really be a loop and run more than one callback.
		if(!this_thread->_observeQueue.empty()) {
			auto observe = this_thread->_observeQueue.removeFront();
			assert(this_thread->_observeQueue.empty());
			observe->trigger(Error::kErrSuccess, kIntrStop);
		}

		assert(!intsAreEnabled());
		runDetached([] {
			ScheduleGuard schedule_lock(scheduleLock.get());
			doSchedule(frigg::move(schedule_lock));
		});
	}
}

void Thread::activateOther(frigg::UnsafePtr<Thread> other_thread) {
	assert(other_thread->_runState == kRunSuspended
			|| other_thread->_runState == kRunDeferred);
	other_thread->_runState = kRunActive;
}

void Thread::unblockOther(frigg::UnsafePtr<Thread> thread) {
	auto lock = frigg::guard(&thread->_mutex);
	if(thread->_runState != kRunBlocked)
		return;

	thread->_runState = kRunDeferred;
	{
		ScheduleGuard schedule_lock(scheduleLock.get());
		enqueueInSchedule(schedule_lock, thread);
	}
}

void Thread::resumeOther(frigg::UnsafePtr<Thread> thread) {
	auto lock = frigg::guard(&thread->_mutex);
	assert(thread->_runState == kRunInterrupted);

	thread->_runState = kRunSuspended;
	{
		ScheduleGuard schedule_lock(scheduleLock.get());
		enqueueInSchedule(schedule_lock, thread);
	}
}

Thread::Thread(KernelSharedPtr<Universe> universe,
		KernelSharedPtr<AddressSpace> address_space)
: flags(0), _runState(kRunInterrupted),
		_numTicks(0), _activationTick(0),
		_pendingSignal(kSigNone), _runCount(1),
		_context(kernelStack.base()),
		_universe(frigg::move(universe)), _addressSpace(frigg::move(address_space)) {
//	frigg::infoLogger() << "[" << globalThreadId << "] New thread!" << frigg::endLog;
	auto stream = createStream();
	_superiorLane = frigg::move(stream.get<0>());
	_inferiorLane = frigg::move(stream.get<1>());
}

Thread::~Thread() {
	if(!_observeQueue.empty())
		frigg::infoLogger() << "\e[35mFix thread destructor!\e[39m" << frigg::endLog;
}

Context &Thread::getContext() {
	return _context;
}

KernelUnsafePtr<Universe> Thread::getUniverse() {
	return _universe;
}
KernelUnsafePtr<AddressSpace> Thread::getAddressSpace() {
	return _addressSpace;
}

void Thread::signalStop() {
	assert(_pendingSignal == kSigNone);
	_pendingSignal = kSigStop;
}

void Thread::_blockLocked(frigg::LockGuard<Mutex> lock) {
	auto this_thread = getCurrentThread();
	assert(lock.protects(&this_thread->_mutex));
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunBlocked;

	assert(!intsAreEnabled());
	if(forkExecutor()) {
		runDetached([] (frigg::LockGuard<Mutex> lock) {
			// TODO: exit the current thread.
			lock.unlock();

			ScheduleGuard schedule_lock(scheduleLock.get());
			doSchedule(frigg::move(schedule_lock));
		}, frigg::move(lock));
	}
}

} // namespace thor

