#include "base.h"

#include <thread>

#include "folder/folder.h"
#include "process/process.h"
#include <iostream>

Base::Base()
{
	window.Init();
	FolderManager::GetCutieFolder();

	ProcessManager::ExtractEmbeddedPayloads();
}

void Base::Run()
{
	while (window.Update())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

void Base::Cleanup()
{
	window.Cleanup();
}

bool BaseUtils::IsDllUpdated()
{
	
	return true;
}

bool BaseUtils::IsInjectorUpdated()
{
	
	return true;
}

bool BaseUtils::UpdateDll(std::string oldPath)
{
	return false;
}

bool BaseUtils::UpdateInjector()
{
	return false;
}
