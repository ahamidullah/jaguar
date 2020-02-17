Semaphore CreateSemaphore(u32 initialValue)
{
	sem_t semaphore;
	sem_init(&semaphore, 0, initialValue);
	return semaphore;
}

void SignalSemaphore(Semaphore *semaphore)
{
	sem_post(semaphore);
}

void WaitOnSemaphore(Semaphore *semaphore)
{
	sem_wait(semaphore);
}

s32 GetSemaphoreValue(Semaphore *semaphore)
{
	s32 value;
	sem_getvalue(semaphore, &value);
	return value;
}
