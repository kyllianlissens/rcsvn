#include "stdafx.h"
#include "TSMK2.h"
#include "safestr.h"
#include "vkmgr.h"
#include "utils.h"
#include "mmgr.h"
#include <memory>

RBX::TSMK2Resolver *t2r;

std::vector <RBX::Task*> RBXTasks;

typedef int(__thiscall *ScriptsJob_Def)();
ScriptsJob_Def ScriptsJob_T;

typedef int(__thiscall *HashChecker_Def)();
HashChecker_Def HashChecker_T;

typedef int(__thiscall *PktRecvJob_Def)();
PktRecvJob_Def PktRecvJob_T;

VirtualKeyMgr Tele('T');

int HashCounter = 0;

RBX::TSMK2Resolver::TSMK2Resolver()
{
	retchk = false;
	recvpkts = false;
	genericJob = nullptr;
	packetRecvJob = nullptr;
	queue_loc = 0;
	pktrecv = 0;
}

VOID RBX::TSMK2Resolver::ResetVars()
{
	TsException = true;
	packetRecvJob = nullptr;
	genericJob = nullptr;
}

VOID RBX::TSMK2Resolver::ReinitializeHook()
{
	if (localPlayer)
	{
		// check localplayer to see if a teleport actually occured

		uintptr_t newLocalPlayer = *(uintptr_t*)(players + GETLP_OFF);

		if (localPlayer == newLocalPlayer)
		{

			//redefine datamodel//
			datamodel = ModBaseAddr + Game_Ptr_A;
			datamodel = *reinterpret_cast<uintptr_t*>(datamodel);

			if (!datamodel)	/* check datamodel validity */
			{
				datamodel = ModBaseAddr + Game_Ptr_B;
				datamodel = *reinterpret_cast<uintptr_t*>(datamodel);
			}

			// redefine workspace //
			workspace = datamodel + gserv_Workspace_Offset;
			workspace = *reinterpret_cast<uintptr_t*>(workspace);

			// connect hook //
			if (workspace)
			{
				game = ins.GetParent(workspace);
				players = (uintptr_t)ins.GetChildByClass(game, "Players");
			}

			return;
		}
	}

	// job queue valid? //
	int gJobindex = t2r->GetDummyJob();

	if (gJobindex < 0)
		gJobindex = t2r->GetDummyJob(0); // try nullptr

	if (gJobindex > -1)
	{

		//redefine datamodel//
		datamodel = ModBaseAddr + Game_Ptr_A;
		datamodel = *reinterpret_cast<uintptr_t*>(datamodel);

		if (!datamodel)	/* check datamodel validity */
		{
			datamodel = ModBaseAddr + Game_Ptr_B;
			datamodel = *reinterpret_cast<uintptr_t*>(datamodel);
		}

		// redefine workspace //
		workspace = datamodel + gserv_Workspace_Offset;
		workspace = *reinterpret_cast<uintptr_t*>(workspace);

		// connect hook //
		if (workspace)
		{
			game = ins.GetParent(workspace);
			players = (uintptr_t)ins.GetChildByClass(game, "Players");

			localPlayer = *(uintptr_t*)(players + GETLP_OFF);

			t2r->HookScriptJob();
			t2r->HookMemoryJob();
			t2r->HookPacketRecvJob();
		}
	}
}

std::vector <std::shared_ptr<const RBX::TaskScheduler::Job>>&
RBX::TSMK2Resolver::GetJobQueue()
{
	auto que_ptr = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)) + QueRootPtr;
	que_ptr = *reinterpret_cast<uintptr_t*>(que_ptr);
	que_ptr = *reinterpret_cast<uintptr_t*>(que_ptr + QueVecPtr);
	que_ptr = *reinterpret_cast<uintptr_t*>(que_ptr + queue_loc);
	que_ptr = *reinterpret_cast<uintptr_t*>(que_ptr + QueVec3Ptr);

	if (que_ptr)
	{
		que_ptr += 4;

		auto& vec = *reinterpret_cast<std::vector<std::shared_ptr<const RBX::TaskScheduler::Job> >*>(que_ptr);

		return vec;
	}

	std::vector <std::shared_ptr<const RBX::TaskScheduler::Job>> vec;

	return vec;
}

DWORD RBX::TSMK2Resolver::GetPacketRecvJob(std::vector <std::shared_ptr<const RBX::TaskScheduler::Job>>& vec)
{
	std::string PktRecvStr("Net PacketReceive");

	for (size_t i = 0; i < vec.size(); i++)
	{
		if (vec[i]->name == PktRecvStr)
		{
			//packetRecvJob = vec[i];
			return i;
		}
	}

	return -1;
}

DWORD RBX::TSMK2Resolver::GetMemoryCheckerJob(std::vector <std::shared_ptr<const RBX::TaskScheduler::Job>>& vec)
{
	std::string MCheckStr("US14116");

	for (size_t i = 0; i < vec.size(); i++)
	{
		if (vec[i]->name == MCheckStr)
		{
			return i;
		}
	}

	return -1;
}

VOID RBX::TSMK2Resolver::ThrottlePacketJob()
{
	recvpkts = !recvpkts;
}

DWORD RBX::TSMK2Resolver::GetDummyJob(size_t vecptr)
{
	auto que_ptr = reinterpret_cast  <uintptr_t>(GetModuleHandle(nullptr)) + QueRootPtr;
	que_ptr = *reinterpret_cast <uintptr_t*>(que_ptr);
	que_ptr = *reinterpret_cast <uintptr_t*>(que_ptr + QueVecPtr);
	que_ptr = *reinterpret_cast <uintptr_t*>(que_ptr + vecptr);
	que_ptr = *reinterpret_cast <uintptr_t*>(que_ptr + QueVec3Ptr) + 4;

	auto& vec = *reinterpret_cast<std::vector<std::shared_ptr<const RBX::TaskScheduler::Job> >*>(que_ptr);

	queue_loc = vecptr; // store real ptr path for other methods to use

	std::string GenericJobStr("None Marshalled");

	for (size_t i = 0; i < vec.size(); i++)
	{
		if (vec[i]->name == GenericJobStr)
		{
			genericJob = vec[i];
			return i;
		}
	}

	return -1;
}

int __declspec(naked) PktChecker_Hook()
{
	__asm pushad;
	if (t2r->PktRcvDisabled())
	{
		__asm popad;
		__asm mov eax, 1;
		__asm ret 4;
	}
	__asm popad;
	__asm jmp PktRecvJob_T;
}

VOID RBX::TSMK2Resolver::HookPacketRecvJob()
{
	auto &vec = GetJobQueue();

	std::string PktRecvStr("Net PacketReceive");

	for (size_t i = 0; i < vec.size(); i++)
	{
		if (vec[i]->name == PktRecvStr)
		{
			PktRecvJob_T = (PktRecvJob_Def)GetVirtualFunction(vec[i]->vtable, 2);
			
			SwapVTable(vec[i], 0xf00, (uintptr_t*)PktChecker_Hook);

			packetRecvJob = vec[i];
		}
	}
}

VOID RBX::TSMK2Resolver::HookScriptJob()
{
	auto &vec = GetJobQueue();

	std::string ScriptsJob("WaitingScriptJob");

	for (size_t i = 0; i < vec.size(); i++)
	{
		if (vec[i]->name == ScriptsJob)
		{
			SwapVTable(vec[i], 0xf00);
		}
	}
}

VOID RBX::TSMK2Resolver::HookMemoryJob()
{
	auto &vec = GetJobQueue();

	std::string MCheckStr("US14116");

	for (size_t i = 0; i < vec.size(); i++)
	{
		if (vec[i]->name == MCheckStr)
		{
			HookMemCheckVFT(vec[i], 0xf00);
		}
	}
}

void RBX::TSMK2Resolver::DisableMemCheck()
{
	retchk = true; // also memcheck flag;
}

void RBX::TSMK2Resolver::EnableMemCheck()
{
	retchk = false; // also memcheck flag
}

void RBX::TSMK2Resolver::BypassChecks()
{
	Mv2->write_memory(retcheck_1, "\xEB");

	retchk = true;

	Sleep(60);
}

void RBX::TSMK2Resolver::ResumeChecks()
{
	Mv2->write_memory(retcheck_1, "\x72");

	retchk = false;
}

bool RBX::TSMK2Resolver::RetChkDisabled()
{
	return retchk;
}

bool RBX::TSMK2Resolver::PktRcvDisabled()
{
	return recvpkts;
}

int __declspec(naked) HashChecker_Hook()
{
	__asm pushad;
	__asm inc dword ptr [HashCounter]; 	// ping hook
	if (t2r->RetChkDisabled())
	{	
		__asm popad;
		__asm mov eax, 1;
		__asm ret 4;
	}
	__asm popad;
	__asm jmp HashChecker_T;
}

VOID RBX::TSMK2Resolver::HookMemCheckVFT(
	std::shared_ptr <const RBX::TaskScheduler::Job> Job,
	size_t szVtable)
{
	// virtual function def

	HashChecker_T = (HashChecker_Def)GetVirtualFunction(Job->vtable, 2);

	// allocate memory for vftable clone

	auto m_new_vtable = new uintptr_t[szVtable];

	// copy the virtual table to our clone

	memcpy(m_new_vtable, Job->vtable, szVtable);

	// detour virtual function @$+8

	m_new_vtable[2] = (uintptr_t)(HashChecker_Hook);

	// point job to new vtable

	auto jobptr = *reinterpret_cast <uintptr_t*>(&Job);
	*reinterpret_cast<uintptr_t*>(jobptr) = (uintptr_t)m_new_vtable;
}

void __fastcall TeleportImpl()
{
	if (Tele.Pressed())
	{
		try 
		{
			t2r->BypassChecks();

			RbxMouse newMouse;

			GetPlayerMouse(localPlayer, &newMouse);

			mouse = newMouse.mouse_t[0];

			CFrame newCFrame;
			GetMouseHit(mouse, &newCFrame);

			float pX = newCFrame.position.x,
				  pY = newCFrame.position.y,
				  pZ = newCFrame.position.z;

			character = *(uintptr_t*)(localPlayer + GETCHRCTR_OFF);

			uintptr_t hum = NULL;

			if (character)
				hum = ins.GetChildByClass(character, "Humanoid");
		
			if (hum)
			{
				uintptr_t healthptr = *(uintptr_t*)(hum + PLRHEALTH_OFF);
				uintptr_t encoded = *(uintptr_t*)(healthptr) ^ healthptr;

				float health = *(float*)&encoded;

				if (health > 0.0)
				{
					MoveTo(character, pX, pY, pZ);
				}

			}

			t2r->ResumeChecks();

		}
		catch (...)
		{
			std::cout << "Error: " << GetLastError() << "\n";
		}
		
	}
}

int __fastcall ScriptsJob_Hook() {
	__asm pushad // Save registers on stack 
	TeleportImpl(); // Teleport user on key press //
	querytasks(); // Execute tasks in the queue //
	__asm popad  // Restore registers from stack
	return ScriptsJob_T();
}

VOID RBX::TSMK2Resolver::SwapVTable(
	std::shared_ptr <const RBX::TaskScheduler::Job> Job,
	size_t szVtable, uintptr_t* func2hook)
{

	// allocate memory for vftable clone

	auto m_new_vtable = new uintptr_t[szVtable];

	// copy the virtual table to our clone

	memcpy(m_new_vtable, Job->vtable, szVtable);

	// detour virtual function @$+8

	m_new_vtable[2] = (uintptr_t)func2hook;

	// point job to new vtable

	auto jobptr = *reinterpret_cast <uintptr_t*>(&Job);
	*reinterpret_cast<uintptr_t*>(jobptr) = (uintptr_t)m_new_vtable;
}

VOID RBX::TSMK2Resolver::SwapVTable(
	std::shared_ptr <const RBX::TaskScheduler::Job> Job,
	size_t szVtable) 
{
	// virtual function def

	ScriptsJob_T = (ScriptsJob_Def)GetVirtualFunction(Job->vtable, 2);

	// allocate memory for vftable clone

	auto m_new_vtable = new uintptr_t[szVtable];

	// copy the virtual table to our clone

	memcpy(m_new_vtable, Job->vtable, szVtable);

	// detour virtual function @$+8

	m_new_vtable[2] = (uintptr_t)(ScriptsJob_Hook);

	// point job to new vtable

	auto jobptr = *reinterpret_cast <uintptr_t*>(&Job);
	*reinterpret_cast<uintptr_t*>(jobptr) = (uintptr_t)m_new_vtable;
}

DWORD WINAPI JobHook(LPVOID)
{
	// Find appropriate pointer path & generic job

	int gJobindex = t2r->GetDummyJob();

	if (gJobindex < 0)
		gJobindex = t2r->GetDummyJob(0); // try nullptr

	if (gJobindex > -1)
	{
		t2r->HookScriptJob();
		t2r->HookMemoryJob();
		t2r->HookPacketRecvJob();  
	}
	else {
		return 0;
	}

	int prvHashCounter= 0; // dynamic ping value //

	// re-hook after teleport //

	for (;;)
	{
		// check if our hook is attached //
		if (prvHashCounter != HashCounter)
		{
			prvHashCounter = HashCounter;
			
			// wait some time for ping value to change //
			Sleep(5000); 
			
			// if ping value doesn't change, reconnect our hook
			if (prvHashCounter == HashCounter)
			{
				HashCounter = 0;
				__try
				{
					t2r->ReinitializeHook();
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					// Resolve pointer after teleportation or error

					DWORD dw = GetExceptionCode();

					// std::cout << "Exception code: " << dw << "\n";

					t2r->ResetVars();
				}
			}
		}

		if (HashCounter == NULL)
		{
			// trigger ping check
			prvHashCounter = 1;
		}

		Sleep(1000);
	}

	return 0;
}

DWORD WINAPI TSMK2Throttler(LPVOID lpParam)
{
	VirtualKeyMgr HotKey_B('B');

	for (;;)
	{
		if (HotKey_B.Pressed())
		{
			t2r->ThrottlePacketJob();
		}

		Sleep(30);
	}

	return 0;
}

int RBX::TSMK2Resolver::GetVirtualFunction
(void *vtable, int index) const {
	return *reinterpret_cast<uintptr_t*>((uintptr_t)vtable + (index * 4));
}

void querytasks()
{
	try 
	{	
		const size_t sz_tasks = RBXTasks.size();

		for (size_t i = 0; i < sz_tasks; i++)
		{
			RBX::Task *rt = RBXTasks[i];

			if (rt->active)
			{
				rt->active = false;	 // remove if you want the task to run forever

				const void *self = rt->args[0];
				const void *callfunc = rt->function;
				const size_t n_args = rt->args.size();

				if (rt->type == _thisCall)
				{
					// itr == 1, since first arg in ecx
					for (size_t itr = 1; itr < n_args; itr++)
					{
						void *varargs = rt->args[itr];
						__asm push varargs;
					};
					__asm {
						__asm mov ecx, self;
						__asm mov ecx, [ecx];
						call callfunc;
					}
				}
				else {
					for (size_t itr = 0; itr < n_args; itr++)
					{
						void *varargs = rt->args[itr];
						__asm push varargs;
					};
					__asm call callfunc;
				}
			}
			else {
				// clean up dead jobs if you wanna
			}
		}
	}
	catch (...)
	{
		std::cout << "Error: " << GetLastError() << "\n";
	}
}

void __fastcall RBX::TSMK2Resolver::addTask
(uintptr_t* F, std::vector<void*> _args, Call_T _type)
{
	RBX::Task *newTask = new RBX::Task;
	
	newTask->function = F;
	newTask->type = _type;
	newTask->args = _args;
	newTask->active = true;

	RBXTasks.emplace_back(newTask);
}