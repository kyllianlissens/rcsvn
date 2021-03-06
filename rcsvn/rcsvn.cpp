// rcsvn.cpp : Here lies the code for RC7 bytecode conversion  //
// * Comments are well placed for you throughout the application //
// * Copyright (c) 2019 Cheat Buddy

#include "rcsvn.h"

#define vm_key_local 0x30  // vm_key @[ebp-xx]

RBX::Serializer::VanillaD *Rs; // Serializer object

CreateDString_Def CreateDString; // Allocates mem for deserialzier

flat_hash_map<std::string, uint64_t> strings, _instrcache; // String table (string, var_int(id)) // Cache

uintptr_t LoadScriptJumpBack, VMJumpBack; // Return to deserializer // Return to vm

/* allows us to skip pass encryptions and checks in the vm */
uintptr_t OP_RETURN_HOOK, OP_CALL_HOOK, OP_MOVE_HOOK, OP_SETUPVAL_HOOK,
		  OP_JMP_HOOK, OP_CLOSURE_HOOK;

std::mutex mtx; /* _instrcache protection */

/*
// create an offset scanner for our current offsets
// create an auto scanner for l_closure
// create auto decryption/encryption methods
// Mv2->write_key_check(vm_key_local);
*/

using namespace LuaLoad;

int Writer(lua_State *L, const void* b, size_t size, void* B) {
	(void)L;
	luaL_addlstring((luaL_Buffer*)B, (const char *)b, size);
	return 0;
}

int str_dump(lua_State *L) {
	luaL_Buffer b;
	luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	luaL_buffinit(L, &b);
	lua_dump(L, Writer, &b);
	luaL_pushresult(&b);
	return 1;
}

VOID RBX::Serializer::VanillaD::InitDump() {
	rbx_dump = VirtualAlloc(0, 8192, MEM_COMMIT, PAGE_READWRITE);
}

RBX::Serializer::VanillaD::VanillaD()
{
	void *sz_dump = nullptr;
	void *rbx_dump = nullptr;
	szlocout = 0;
	lprotoPtr = 0;
	/* point to instructions */
	ldumpPtr = 5;
	ldbgptr = 0;
	scnlen = 0;
	sizek = 0;
}

uint64_t OpDuplicate(std::string luaOp)
{
	std::lock_guard<std::mutex> guard(mtx);

	uint64_t Id = _instrcache.size();

	if (_instrcache[luaOp])
	{
		// instr exists, return its index
		return _instrcache[luaOp];
	}
	else {
		// instr doesn't exist add to cache
		_instrcache[luaOp] = Id;
	}

	return 0;
}

uint64_t IsDuplicate(std::string luaStr, uint64_t Id)
{
	if (strings[luaStr])
	{
		// string exists, return its index
		return strings[luaStr];
	}
	else {
		// string doesn't exist add to strings table
		strings[luaStr] = Id;
	}
	return 0;
}

VOID RBX::Serializer::VanillaD::ConvertDump(const char *lProto, int *cProptr)
{
	/* check for precompiled code (`<esc>Lua') */
	memcpy(&HEADER, &LUA_SIGNATURE[0], 4);

	bool ltopProto;	// ;; Proto level

	if (*(DWORD*)&lProto[0] == HEADER)
		ltopProto = true;
	else
		ltopProto = false;

	/* source name length */
	memcpy(&scnlen, lProto + lScnlenPtr, 4);

	size_t sizep; // ;; Number of function prototypes

	if (ltopProto)
	{
		/* retrieve var_arg flag and max_stack from top level function */
		memcpy(&isvararg, lProto + scnlen + ltopVarPtr, 1);
		memcpy(&maxstack, lProto + scnlen + ltopStkPtr, 1);

		/* point to top level instruction list */
		lprotoPtr = scnlen + ltopCodePtr;
	}
	else
	{
		/* retrieve params, var_arg, and max_stack from protos */
		memcpy(&numupvals, lProto + lUpvalsPtr, 1);
		memcpy(&numparams, lProto + lParamsPtr, 1);
		memcpy(&isvararg, lProto + lProVarPtr, 1);
		memcpy(&maxstack, lProto + lProStkPtr, 1);

		/* point to proto instruction list */
		lprotoPtr = lproCodePtr;
	}

	/* encode no. params with vlq */
	size_t sz_pa_out = encodeVarint<uint64_t>(numparams, pa_enc);

	/* encode no. upvals with vlq */
	size_t sz_up_out = encodeVarint<uint64_t>(numupvals, u_enc);

	/* number of instructions */
	memcpy(&num_op, lProto + lprotoPtr, 4);

	/* encode no. opcodes with vlq */
	size_t sz_op_out = encodeVarint<uint64_t>(num_op, op_enc);

	/* convert vanilla instructions to rlua */
	for (uint32_t i = 1; i <= num_op; i++)
	{
		/* op_idx is instruction index */
		size_t op_idx = lprotoPtr + (i * 4);
		memcpy(&Instruction, lProto + op_idx, 4);

		/* Convert Opcode to Rlua */
		Opcode = (ConvertOp(Instruction, i));

		/* add instructions to vector */
		op_list.push_back(Opcode);

		char strop[12] = { 0 };
		UlongToHexString(Opcode, strop);

		/* add to cache */
		OpDuplicate(strop);

		/* adjust lproto pointer */
		if (num_op == i)
			lprotoPtr = op_idx + 4;
	}

	/* get number of constants */
	memcpy(&num_cs, lProto + lprotoPtr, 4);

	/* encode with vlq */
	size_t sz_cs_out = encodeVarint<uint64_t>(num_cs, c_enc);

	uint32_t typeptr = 4;  /* points to constant type */
	uint32_t luatype = 0;  /* constant type */
	uint32_t size_c = 0;   /* constant size */

	/* traverse constant table */
	for (uint32_t i = 1; i <= num_cs; i++)
	{
		/* cs_idx is constant index */
		size_t cs_idx = lprotoPtr + typeptr;

		memcpy(&luatype, lProto + cs_idx, 1);

		double luaNum = 0.0f;  /* luaNumber */

		std::string luaStr;    /* luaString */

		/* store constants to Kst table */
		switch (luatype)
		{
		case LUA_TBOOLEAN:
			memcpy(&luatype, lProto + cs_idx + 1, 1);
			if (luatype == 0)
				writeByte(Kst, R_LUA_FBOOLEAN);
			else
				writeByte(Kst, R_LUA_TBOOLEAN);
			typeptr += 2;
			break;
		case LUA_TSTRING:
		{
			if (!sizek) sizek = 1;
			writeByte(Kst, R_LUA_TSTRING);
			luaStr = &lProto[cs_idx + 5];

			uint64_t KstId = IsDuplicate(luaStr, sizek);

			if (KstId)
			{
				writeVarInt(Kst, KstId);
			}
			else {
				writeVarInt(Kst, sizek);
				sizek++;
			}

			memcpy(&size_c, lProto + cs_idx + 1, 1);
			typeptr += size_c + 5;
			break;
		}
		case LUA_TNUMBER:
			writeByte(Kst, R_LUA_TNUMBER);
			memcpy(&luaNum, lProto + cs_idx + 1, 8);
			writeDouble(Kst, luaNum);
			typeptr += 9;
			break;
		case LUA_TNIL:
			writeByte(Kst, R_LUA_TNIL);
			typeptr += 1;
			break;
		default:
			break;
		}
	}

	/* point to protos */
	lprotoPtr += typeptr;

	/* get number of prototypes */
	memcpy(&sizep, lProto + lprotoPtr, 4);

	/* encode with vlq */
	size_t sz_proto_out = encodeVarint<uint64_t>(sizep, p_enc);

	/* lprotoPtr copy */
	size_t lprotoPtrCopy = lprotoPtr + 4;

	if (sizep == NULL)
	{
		lprotoPtr += 4;	/* point to source line positions */
		size_t srclines = EncryptLines(lProto + lprotoPtrCopy);
		lprotoPtrCopy += (4 * srclines) + 4; /* point to locals */
		lprotoPtrCopy += CopyLocals(lProto + lprotoPtrCopy);
		/* point to upvalues */
		lprotoPtrCopy += 4;
		lprotoPtrCopy += CopyUpvals(lProto + lprotoPtrCopy);
		lprotoPtrCopy += 4;
		/* end of function block */
	}
	else {
		lprotoPtr += 4;	/* point to start of proto */

		/* Set lprotoptrCopy to debug data */
		for (uint32_t i = 1; i <= sizep; i++) {
			lprotoPtrCopy += GetLineInfo(lProto + lprotoPtrCopy);
		}

		/* point to source line positions */
		size_t srclines = EncryptLines(lProto + lprotoPtrCopy);

		/* point to locals */
		lprotoPtrCopy += (4 * srclines) + 4;

		lprotoPtrCopy += CopyLocals(lProto + lprotoPtrCopy);

		/* point to upvalues */
		lprotoPtrCopy += 4;

		lprotoPtrCopy += CopyUpvals(lProto + lprotoPtrCopy);

		lprotoPtrCopy += 4;
	}

	if (cProptr)
		*(int*)cProptr = lprotoPtrCopy; // end of current proto

	HeaderData *hdr = new HeaderData;

	hdr->szconst = sz_cs_out;
	hdr->szopcodes = sz_op_out;
	hdr->szparams = sz_pa_out;
	hdr->szproto = sz_proto_out;
	hdr->szupvals = sz_up_out;

	WriteHeader(hdr);

	delete hdr;

	WriteDump();

	size_t protoLevel = lprotoPtr;

	/* Convert function prototypes */
	for (uint32_t i = 1; i <= sizep; i++) {
		int protolen = 0;
		ConvertDump(lProto + protoLevel, &protolen);
		protoLevel += protolen;
	}

	if (ltopProto)
	{
		/* encode no. strings with vlq */
		size_t strings_out = encodeVarint<uint64_t>(strings.size(), k_enc);

		/* end of dump ptr */
		uint32_t eof = ldumpPtr - 1;

		/* write size to header */
		memcpy(reinterpret_cast<char*>(rbx_dump) + 1, &eof, 4);

		/* Write strings to dump */
		memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr,
			&k_enc, strings_out);

		ldumpPtr += strings_out;

		/* sort strings list */
		std::vector<std::pair<std::string, uint64_t>>
			elems(strings.begin(), strings.end());

		std::sort(elems.begin(), elems.end(), [](auto &left, auto &right) {
			return left.second < right.second;
		});

		/* write string length to dump */
		for (const auto&n : elems)
		{
			strings_out = encodeVarint<uint64_t>(n.first.length(), k_enc);

			memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr,
				&k_enc, strings_out);

			ldumpPtr += strings_out;
		}

		/* write corresponding strings to dump */
		for (const auto&n : elems)
		{
			memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr,
				&n.first[0], n.first.length());

			ldumpPtr += n.first.length();
		}

		/* reset strings list */
		strings.clear();
	}

}

VOID RBX::Serializer::VanillaD::WriteDump()
{
	std::string stuff = Kst.str();

	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr,
		&stuff[0], stuff.size());

	ldumpPtr += stuff.size();

	stuff = slines.str();
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr,
		&stuff[0], stuff.size());

	ldumpPtr += stuff.size();

	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr,
		&op_list[0], op_list.size() * 4);

	ldumpPtr += op_list.size() * 4;

	Kst.str(std::string());
	slines.str(std::string());
	slines.clear();
	op_list.clear();
	Kst.clear();
}

size_t RBX::Serializer::VanillaD::GetLineInfo(const char* Dump)
{
	// --Traverse proto list to find top level source line info-- //

	l_Proto cp; // current proto //

	/* Top level lines ptr */
	unsigned int linesptr = cp.insptr;

	memcpy(&cp.sizeop, Dump + cp.insptr, 4); /* number of instructions */
	linesptr += (cp.sizeop + 1) * 4;		 /* point to constants */

	/* Number of constants */
	memcpy(&cp.sizecs, Dump + linesptr, 4);

	/* Traverse constants */
	for (uint32_t i = 1; i <= cp.sizecs; i++)
	{
		memcpy(&cp.luatype, Dump + linesptr + (cp.consptr), 1);
		memcpy(&cp.cslen, Dump + linesptr + (cp.consptr + 1), 4);
		if (cp.luatype == LUA_TBOOLEAN)
		{
			cp.consptr += 2;
			continue;
		}
		if (cp.luatype == LUA_TNUMBER)
		{
			cp.consptr += 9;
			continue;
		}
		if (cp.luatype == LUA_TNIL)
		{
			cp.consptr += 1;
			continue;
		}
		cp.consptr += 5 + cp.cslen;
	}

	linesptr += cp.consptr;

	/* number of protos */
	memcpy(&cp.protos, Dump + linesptr, 4);

	/* start of proto if any */
	linesptr += 4;

	if (cp.protos != 0)
	{
		for (uint32_t i = 1; i <= cp.protos; i++)
		{
			linesptr += GetLineInfo(Dump + linesptr);
		}
	}

	/* source line list */
	memcpy(&cp.size_sl, Dump + linesptr, 4);
	linesptr += 4;
	/* point to locals */
	linesptr += cp.size_sl * 4;
	/* number of locals */
	memcpy(&cp.locals, Dump + linesptr, 4);

	/* string length */
	size_t sz_local = 0, sz_upval = 0;

	for (uint32_t i = 1; i <= cp.locals; i++)
	{
		linesptr += sz_local + 4;
		memcpy(&sz_local, Dump + linesptr, 4);
		sz_local += 8;
	}

	linesptr += sz_local + 4;
	/* upvalues list */
	memcpy(&cp.upvals, Dump + linesptr, 4);
	linesptr += 4;

	for (uint32_t i = 1; i <= cp.upvals; i++)
	{
		/* size of upvalue */
		memcpy(&sz_upval, Dump + linesptr, 4);
		linesptr += sz_upval + 4;
	}

	return linesptr;
}

size_t RBX::Serializer::VanillaD::CopyUpvals(const char* Dump)
{
	std::string luaStr;
	size_t sz_upval = 0, Dptr = 0u;

	for (uint32_t i = 1; i <= numupvals; i++) {

		sz_upval = Dump[4];
		luaStr = &Dump[8];

		uint64_t KstId = strings[luaStr];

		writeVarInt(slines, KstId);

		Dump += (sz_upval + 4);
		Dptr += (sz_upval + 4);
	}

	return Dptr;

}

size_t RBX::Serializer::VanillaD::CopyLocals(const char* Dump)
{
	std::string luaStr;
	size_t sz_local = 0, Dptr = 0u;

	memcpy(&num_ls, Dump, 4);

	/* encode with vlq */
	szlocout = encodeVarint<uint64_t>(num_ls, l_enc);

	for (uint32_t i = 1; i <= num_ls; i++)
	{
		if (!sizek) sizek = 1;

		sz_local = Dump[4];
		luaStr = &Dump[8];

		uint64_t KstId = IsDuplicate(luaStr, sizek);

		DWORD startpc = Dump[sz_local + 8];
		DWORD endpc = Dump[sz_local + 12];

		if (KstId)
		{
			writeVarInt(slines, startpc);
			writeVarInt(slines, endpc);
			writeVarInt(slines, KstId);
		}
		else {
			writeVarInt(slines, startpc);
			writeVarInt(slines, endpc);
			writeVarInt(slines, sizek);
			sizek++;
		}

		Dump += (sz_local + 12);
		Dptr += (sz_local + 12);
	}

	return Dptr;
}

size_t RBX::Serializer::VanillaD::EncryptLines(const char* Dump)
{
	DWORD instrline = NULL,
		prevLine = NULL;

	size_t srclines; // ;; Size of source line positions

	memcpy(&srclines, Dump, 4);

	Dump += 4;

	for (uint32_t i = 0; i < srclines; i++)
	{
		memcpy(&instrline, Dump + (i * 4), 4);

		instrline = instrline ^ (i << 8);
		DWORD result = instrline;
		instrline -= prevLine;
		prevLine = result;

		writeVarInt(slines, instrline);
	}

	return srclines;

}

VOID RBX::Serializer::VanillaD::WriteHeader(RBX::Serializer::HeaderData *hdr)
{
	const uint32_t sz_proto_out = hdr->szproto,
		sz_pa_out = hdr->szparams,
		sz_cs_out = hdr->szconst,
		sz_op_out = hdr->szopcodes,
		sz_up_out = hdr->szupvals;

	memset(rbx_dump, 0x10, 1);

	/* start of no. protos */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &p_enc, sz_proto_out);
	ldumpPtr += sz_proto_out;
	/* start of no. constants */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &c_enc, sz_cs_out);
	ldumpPtr += sz_cs_out;
	/* start of no. opcodes */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &op_enc, sz_op_out);
	ldumpPtr += sz_op_out;
	/* start of no. locals */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &l_enc, szlocout);
	ldumpPtr += szlocout;
	/* start of no. opcodes */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &op_enc, sz_op_out);
	ldumpPtr += sz_op_out;
	/* start of no. upvalues */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &u_enc, sz_up_out);
	ldumpPtr += sz_up_out;
	/* start of maxstack */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &maxstack, 1);
	ldumpPtr += 1;
	/* start of vararg */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &isvararg, 1);
	ldumpPtr += 1;
	/* start of no. upvalues */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &u_enc, sz_up_out);
	ldumpPtr += sz_up_out;
	/* start of no. params */
	memcpy(reinterpret_cast<char*>(rbx_dump) + ldumpPtr, &pa_enc, sz_pa_out);
	ldumpPtr += sz_pa_out;
	/* skip bytecode hash */
	ldumpPtr += 4; //new protos has hash too?;
}

DWORD RBX::Serializer::VanillaD::SizeOfChunk() {
	return ldumpPtr;
}

VOID* RBX::Serializer::VanillaD::GetChunk() {
	return rbx_dump;
}

RBX::Serializer::l_Proto::l_Proto()
{
	sizeop = 0;
	luatype = 0;
	consptr = 4;
	insptr = 16;
	size_sl = 0;
	sizecs = 0;
	upvals = 0;
	protos = 0;
	locals = 0;
	cslen = 0;
}

unsigned char r_get_top()
{
	if (r_lua_gettop)
		return *(BYTE*)((DWORD)r_lua_gettop + 8);
}

unsigned char r_get_base()
{
	if (r_lua_gettop)
		return *(BYTE*)((DWORD)r_lua_gettop + 11);
}

int decrypt_ptr(int loc) {
	return loc ^ *(int*)loc;
}

int encrypt_ptr(int loc, void* value) {
	*(int*)loc = (int)value - loc;
	return loc;
}

void r_incr_top(int r_lua_State)
{
	*(int*)(r_lua_State + r_get_top()) += sizeof(r_TValue);
}

void r_luaC_link(int r_lua_State, int o, byte tt)
{
	int g = decrypt_ptr(r_lua_State + 32);	// global_State *g = G(L)
	*(int*)(o) = *(int*)(g + 32);			// o->gch.next = g->rootgc
	*(int*)(g + 32) = o;					// g->rootgc = o
	*(byte*)(o + 4) = *(byte*)(g + 24) & 3;	// o->gch.marked = luaC_white(g)
	*(byte*)(o + 5) = tt;					// o->gch.tt = tt;
}

r_LClosure* r_luaF_newLclosure(int r_lua_State, int nups, int e) {

	r_LClosure* nlc = (r_LClosure*)
		r_luaM_malloc(r_lua_State, 20 + nups * 4);

	r_luaC_link(r_lua_State, (int)nlc, R_LUA_TFUNCTION);

	nlc->isC = false;
	nlc->env = (int*)e;
	nlc->nupvalues = nups;
	nlc->gclist = 0;

	while (nups--) nlc->upvals[nups] = NULL;
	return nlc;
}

void r_lua_pushLclosure(int r_lua_State, r_LClosure* lc) {
	r_TValue* top = *(r_TValue**)(r_lua_State + r_get_top());
	top->value_0 = (int)lc;
	top->value_1 = 0;
	top->tt = R_LUA_TFUNCTION;
	top->unk = 0;

	r_incr_top(r_lua_State);
}

// --LuaDeserializer::deserialize-- //
int __stdcall ConvertToProto(int r_lua_State, std::string* code, const char* source, int unk)
{

	unsigned char luaMagic = *reinterpret_cast<unsigned char*>(code);// header

	// todo: run regular localscripts with no bytecode
	// todo: protect against descendantadded for coregui, and optional backpack/playergui

	if (luaMagic == 0x10)
	{
		const char* newSource = r_luaS_newlstr(r_lua_State,
											   source,
											   strlen(source)
											);

		// set thread identity

		//

		lDumpInfo *ldi = new lDumpInfo;
		ldi->unk_1 = 1;
		ldi->unk_2 = 15;

		lDumpMetaData md = { 0 };
		md.source = reinterpret_cast<char*>(&code[0]);
		md.eof = md.source + unk;
		md._eof = md.eof;

		ldi->md = &md;

		int *proto = rluaDumpToProto((int)ldi, r_lua_State, newSource, 1);

		int env = *(int*)(r_lua_State + 64);
		r_LClosure* nlc = r_luaF_newLclosure(r_lua_State, 0, env);
		encrypt_ptr((int)&nlc->p, proto);
		r_lua_pushLclosure(r_lua_State, nlc);

		delete ldi;

		return 0;
	}

	return -1;
}

int getstateindex(int _index)
{
	return GetLuaState();

	//--------------------------//
	// deprecated ...			//
	//--------------------------//

	uintptr_t globalState;

	DWORD n_state = _index * 8;

	n_state = n_state - _index;
	n_state = (n_state * 8) + 0xAC;
	n_state = n_state + scriptcontext;

	globalState = *(int*)n_state ^ n_state;

	return globalState;
}

BOOL WINAPI IsOpEncrypted(uintptr_t r_lua_instr)
{
	std::lock_guard<std::mutex> guard(mtx);

	char lu_instr = *(uintptr_t*)r_lua_instr >> (char)26;

	if (lu_instr > 37)
		return 1;

	try {
		for (;;) // inactive lower bit instructions are more likely to trigger a false positive
		{
			if ((*(uintptr_t*)r_lua_instr & 0xfffffff) < 1)
				r_lua_instr += 4; // check next instruction if lower bits aren't active
			else
				break;
		}
	}
	catch (...) {}

	char strop[12] = { 0 };
	UlongToHexString(*(uintptr_t*)r_lua_instr, strop);

	if (_instrcache[strop])	// was created by rc7?
	{
		return 0;
	}

	return 1;
}

void __declspec(naked) vm_hook_src() {
	__asm
	{
		pushad;
		push edi;
		call IsOpEncrypted;
		test al, al;
		popad;
		jne vm_orig;
		mov dword ptr[ebp - vm_key_local], 1;
		/* op_close fix */
		push edx
		shr edx, 26
		cmp edx, 0x20 // op_close
		pop edx
		jne vm_orig
		sub edi, 4
		vm_orig:
		imul edx, [ebp - vm_key_local];
		// update this local weekly
		mov [ebp - 0x28], edi; 
		jmp[VMJumpBack];
	}
}

void PatchOpMove()
{
	for (size_t i = 0; i < 200; i++)
	{
		uintptr_t checkinstr = (OP_MOVE_HOOK + i);

		if (*(BYTE*)checkinstr == 0x74)
		{
			if (*(BYTE*)(checkinstr + 1) == 0x0B)
			{
				Mv2->write_memory(checkinstr, "\x90\x90");
				break;
			}
		}
	}
}

void PatchOpClose()
{
	/* check vm_hook_src for my patch .-. */
}

void PatchOpRet(void *op_ret_hook)
{
	uintptr_t op_ret_jumpback = 0,
		op_ret_start = 0;

	op_ret_start = findOpRetStart(OP_RETURN_HOOK);

	size_t sz_cloned_bytes = op_ret_start - OP_RETURN_HOOK;

	op_ret_jumpback = op_ret_start + 6;
	op_ret_start = Mv2->Disassemble(op_ret_start + 1);

	// copy its original code and append shellcode to skip encryption
	std::memcpy(op_ret_hook,
		reinterpret_cast<void*>(OP_RETURN_HOOK),
		sz_cloned_bytes);

	Mv2->Ja((uintptr_t)op_ret_hook + sz_cloned_bytes,
		// ja op_ret_start(sz: 6)
		op_ret_start, (char*)op_ret_hook + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	Mv2->write_memory((uintptr_t)op_ret_hook + sz_cloned_bytes,
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	Mv2->Jne((uintptr_t)op_ret_hook + sz_cloned_bytes,
		// jne op_ret_jumpback (sz: 6)
		op_ret_jumpback, (char*)op_ret_hook + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	// stub to bypass op_return check by patching thread member @$+58
	Mv2->write_memory((uintptr_t)op_ret_hook + sz_cloned_bytes,
		"\x50\x8b\x43\x58\xc7\x40\x04\x01\x01\x11\x01\x58");

	sz_cloned_bytes += 12;

	Mv2->Jmp((uintptr_t)op_ret_hook + sz_cloned_bytes,
		// jmp op_ret_start (sz: 5)
		op_ret_start, (char*)op_ret_hook + sz_cloned_bytes);

	// Hook OP_RETURN
	MH_CreateHook(reinterpret_cast<void*>
		(OP_RETURN_HOOK), op_ret_hook, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(OP_RETURN_HOOK)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}
}

void PatchOpCall(void *op_call_hook)
{
	uintptr_t op_call_jumpback = 0,
		op_call_start = 0;

	std::stringstream code;

	op_call_start = findOpCallStart(OP_CALL_HOOK);

	std::string r1 = findSourceOperand(op_call_start, "or");

	// shellcode to skip encryption
	code << "mov " + r1 + ", edx;";

	UCHAR asm_buf[5] = { 0 };

	size_t opsize = KsAssembler(&code.str()[0], asm_buf);

	// write memory parsed by keystone
	Mv2->write_memory((uintptr_t)op_call_hook,
		asm_buf, opsize);

	size_t sz_cloned_bytes = opsize;

	Mv2->write_memory((uintptr_t)op_call_hook + sz_cloned_bytes,
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	op_call_jumpback = OP_CALL_HOOK + (op_call_start - OP_CALL_HOOK) + 6;

	Mv2->Je((uintptr_t)op_call_hook + sz_cloned_bytes,
		// je op_call_jumpback (sz: 6)
		op_call_jumpback, (char*)op_call_hook + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	// copy its original code and append to hook
	size_t sz_orig = op_call_start - OP_CALL_HOOK;

	std::memcpy(reinterpret_cast<char*>(op_call_hook) + sz_cloned_bytes,
		reinterpret_cast<void*>(OP_CALL_HOOK), sz_orig);

	sz_cloned_bytes += sz_orig;

	Mv2->Jmp((uintptr_t)op_call_hook + sz_cloned_bytes,
		// jmp op_call_start (sz: 5)
		op_call_start, (char*)op_call_hook + sz_cloned_bytes);

	// Hook OP_CALL to skip encryption
	MH_CreateHook(reinterpret_cast<void*>
		(OP_CALL_HOOK), op_call_hook, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(OP_CALL_HOOK)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}
}

uintptr_t closure_args_jumpback;

__declspec(naked) void closureArgsDetour()
{ /* check op_closure arguments */
	__asm
	{
		cmp dword ptr[eax], 0x6
		je op_move_patch
		cmp dword ptr[eax], 0x2
		je op_getupval_patch
		jmp cl_exit
		op_move_patch :
		mov dword ptr[eax], 0x10 // op_move -> op_add
		jmp cl_exit
		op_getupval_patch :
		mov dword ptr[eax], 0xb  // op_getupval -> op_sub
		cl_exit :
		/* continue to original code */
		cmp dword ptr[eax], 0x10;
		pop eax;
		/* jump back to closure hook */
		jmp dword ptr ds : [closure_args_jumpback]
	}
}

void PatchClosureArgs(void *op_closure_hook)
{
	/*****************************************************************************
	/* patch op_move and op_getupval which is used as args in op_closure		 *
	/* converts op_move -> op_add and op_getupval -> op_sub as used by the client*
	*****************************************************************************/

	std::stringstream code;

	uintptr_t op_closure_jumpback_a = 0,
		op_closure_jumpback_b = 0,
		op_closure_start = 0;

	op_closure_start = findOpClosureUpvalCheck(OP_CLOSURE_HOOK);  // cmp dword ptr [ebp-$], 0x10

	std::string r1 = findSourceOperand(op_closure_start, NULL);

	// remove spaces and h suffix from ebp variable
	r1.erase(std::remove_if(r1.begin(), r1.end(),
		[](char chr) { return chr == 'h' || chr == ' '; }), r1.end()
	);

	std::string r2 = r1.substr(0, 4);
	r2 += "0x" + r1.substr(4);

	// encapsulate with brackets
	r1 = '[' + r2 + ']';

	code << "push eax\n";
	// move local variable being checked into eax
	code << "lea eax, " + r1 + "\n";  

	UCHAR asm_buf[5] = { 0 };

	size_t opsize = KsAssembler(&code.str()[0], asm_buf);

	// write memory parsed by keystone

	Mv2->write_memory((uintptr_t)op_closure_hook,
		asm_buf, opsize);

	op_closure_jumpback_a = Mv2->Disassemble(op_closure_start + 4, false);  // jne jumpback
	op_closure_jumpback_b = op_closure_start + 6; // original code jumpback

	size_t sz_cloned_bytes = opsize;

	// detour to our closure args function
	Mv2->Jmp((uintptr_t)op_closure_hook + sz_cloned_bytes, 	// jmp op__jumpback (sz: 5)
		(uintptr_t)&closureArgsDetour, (char*)op_closure_hook + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	closure_args_jumpback = (uintptr_t)op_closure_hook + sz_cloned_bytes;

	// recreate jne within our cave

	Mv2->Jne((uintptr_t)op_closure_hook + sz_cloned_bytes, 
		// jne op_closure_jumpback_a (sz: 6)
		op_closure_jumpback_a, (char*)op_closure_hook + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	// jump to original code if jne not taken

	Mv2->Jmp((uintptr_t)op_closure_hook + sz_cloned_bytes, 
		// jmp op_closure_jumpback_b (sz: 5)
		op_closure_jumpback_b, (char*)op_closure_hook + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	// Hook OP_CLOSURE arg check
	MH_CreateHook(reinterpret_cast<void*>
		(op_closure_start), op_closure_hook, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(op_closure_start)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}
}

void PatchOpClosure(void *op_closure_hook)
{
	uintptr_t op_closure_jumpback = 0,
		op_closure_start = 0;

	std::stringstream code;

	op_closure_start = findOpClosureStart(OP_CLOSURE_HOOK);

	std::string r1 = findSourceOperand(op_closure_start, "or");

	// shellcode to skip encryption

	UCHAR asm_buf[5] = { 0 };

	code << "mov " + r1 + ", edx;";

	size_t opsize = KsAssembler(&code.str()[0], asm_buf);

	// write memory parsed by keystone

	Mv2->write_memory((uintptr_t)op_closure_hook,
		asm_buf, opsize);

	size_t sz_cloned_bytes = opsize;

	// find and copy locals to prevent crash

	sz_cloned_bytes += appendLocalVar((char*)op_closure_hook
		+ sz_cloned_bytes, OP_CLOSURE_HOOK);

	Mv2->write_memory((uintptr_t)op_closure_hook + sz_cloned_bytes,	
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	op_closure_jumpback = OP_CLOSURE_HOOK + (op_closure_start - OP_CLOSURE_HOOK) + 6;

	Mv2->Je((uintptr_t)op_closure_hook + sz_cloned_bytes,
		// je op_closure_jumpback (sz: 6)
		op_closure_jumpback, (char*)op_closure_hook + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	// restore locals if jump not taken

	code.str(std::string());
	code.clear();

	code << "mov edx, " + r1 + ";";

	opsize = KsAssembler(&code.str()[0], asm_buf);

	// write memory parsed by keystone

	Mv2->write_memory((uintptr_t)op_closure_hook + sz_cloned_bytes,
		asm_buf, opsize);

	sz_cloned_bytes += opsize;

	// copy original code and append to hook

	size_t sz_orig = op_closure_start - OP_CLOSURE_HOOK;

	std::memcpy(reinterpret_cast<char*>(op_closure_hook) + sz_cloned_bytes,
		reinterpret_cast<void*>(OP_CLOSURE_HOOK), sz_orig);

	sz_cloned_bytes += sz_orig;

	// jmp op_closure_start (sz: 5)

	Mv2->Jmp((uintptr_t)op_closure_hook + sz_cloned_bytes,
		op_closure_start, (char*)op_closure_hook + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	// Hook OP_CLOSURE

	MH_CreateHook(reinterpret_cast<void*>
		(OP_CLOSURE_HOOK), op_closure_hook, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>(OP_CLOSURE_HOOK)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}

	PatchClosureArgs((char*)op_closure_hook + sz_cloned_bytes);
}

void PatchOpSetupval(void *op_setupval_hook)
{
	uintptr_t op_setupval_jumpback = 0,
			  op_setupval_start = 0;

	op_setupval_start = findOpSetupvalStart(OP_SETUPVAL_HOOK);

	/* preprend original code to hook */

	size_t sz_cloned_bytes = 5;

	std::memcpy(reinterpret_cast<char*>(op_setupval_hook),
		reinterpret_cast<void*>(OP_SETUPVAL_HOOK), sz_cloned_bytes);

	/* cmp dword ptr [ebp-30], 1 (sz: 7) */

	Mv2->write_memory((uintptr_t)op_setupval_hook + sz_cloned_bytes,
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	op_setupval_jumpback = OP_SETUPVAL_HOOK + (op_setupval_start - OP_SETUPVAL_HOOK);

	Mv2->Je((uintptr_t)op_setupval_hook + sz_cloned_bytes,
		// je op_setupval_jumpback (sz: 6)
		op_setupval_jumpback, (char*)op_setupval_hook + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	Mv2->Jmp((uintptr_t)op_setupval_hook + sz_cloned_bytes,
		// jmp op_setupval_orig (sz: 5)
		OP_SETUPVAL_HOOK + 5, (char*)op_setupval_hook + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	// Hook OP_SETUVAL to skip encryption

	MH_CreateHook(reinterpret_cast<void*>
		(OP_SETUPVAL_HOOK), op_setupval_hook, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(OP_SETUPVAL_HOOK)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}
}

void PatchConditionals(int *vm_table, void *vm_hooks)
{
	//-- Patch OP_JMP in conditionals --//
	uintptr_t op_lt_jumpback_a = 0,
			  op_lt_jumpback_b = 0,
			  op_lt_jumpback_c = 0,
			  op_lt_start = 0;

	uintptr_t OP_LT_HOOK	   = vm_table[0x13];
	uintptr_t OP_LE_HOOK	   = vm_table[0x15];
	uintptr_t OP_TESTSET_HOOK  = vm_table[0x21];
	uintptr_t OP_TFORLOOP_HOOK = vm_table[0x1c];
	uintptr_t OP_TEST_HOOK	   = vm_table[0x17];
	uintptr_t OP_EQ_HOOK	   = vm_table[0x1a];

	op_lt_start = findConditionalStart(OP_LT_HOOK);
	op_lt_jumpback_a = findConditionalJumpback(OP_LT_HOOK) - 5;

	/* get destination of conditional */
	op_lt_jumpback_b = Mv2->Disassemble(op_lt_start + 4);

	/* jump back to original code */
	op_lt_jumpback_c = op_lt_start + 9;

	/* copy original code */
	size_t sz_cloned_bytes = 3;

	std::memcpy(reinterpret_cast<char*>(vm_hooks),
		reinterpret_cast<void*>(op_lt_start), sz_cloned_bytes);

	/* recreate conditional */
	Mv2->Ja((uintptr_t)vm_hooks + sz_cloned_bytes,
		// ja op_lt_jumpback_b(sz: 6)
		op_lt_jumpback_b, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	/* check if unencrypted */
	Mv2->write_memory((uintptr_t)vm_hooks + sz_cloned_bytes,
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");	

	sz_cloned_bytes += 7;

	Mv2->Je((uintptr_t)vm_hooks + sz_cloned_bytes,
		// je op_lt_jumpback_a (sz: 6)
		op_lt_jumpback_a, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	Mv2->Jmp((uintptr_t)vm_hooks + sz_cloned_bytes,
		// jmp op_lt_jumpback_c (sz: 5)
		op_lt_jumpback_c, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	MH_CreateHook(reinterpret_cast<void*>(op_lt_start), 
					vm_hooks, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(op_lt_start)) != MH_OK) {
			std::cout << "Failed to enable hook!" << std::endl;
		return;
	}

	vm_hooks = (char*)vm_hooks + sz_cloned_bytes;

	uintptr_t op_le_jumpback_a = 0,
			  op_le_jumpback_b = 0,
			  op_le_jumpback_c = 0,
			  op_le_start = 0;

	op_le_start = findConditionalStart(OP_LE_HOOK);
	op_le_jumpback_a = findConditionalJumpback(OP_LE_HOOK) - 5;

	/* get destination of conditional */
	op_le_jumpback_b = Mv2->Disassemble(op_le_start + 4);

	/* jump back to original code */
	op_le_jumpback_c = op_le_start + 9;

	/* copy original code */
	sz_cloned_bytes = 3;

	std::memcpy(reinterpret_cast<char*>(vm_hooks),
		reinterpret_cast<void*>(op_le_start), sz_cloned_bytes);

	/* recreate conditional */
	Mv2->Ja((uintptr_t)vm_hooks + sz_cloned_bytes,
		// ja op_le_jumpback_b(sz: 6)
		op_le_jumpback_b, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	/* check if unencrypted */
	Mv2->write_memory((uintptr_t)vm_hooks + sz_cloned_bytes,
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	Mv2->Je((uintptr_t)vm_hooks + sz_cloned_bytes,
		// je op_le_jumpback_a (sz: 6)
		op_le_jumpback_a, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	Mv2->Jmp((uintptr_t)vm_hooks + sz_cloned_bytes,
		// jmp op_le_jumpback_c (sz: 5)
		op_le_jumpback_c, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	MH_CreateHook(reinterpret_cast<void*>(op_le_start),
		vm_hooks, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(op_le_start)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}

	vm_hooks = (char*)vm_hooks + sz_cloned_bytes;

	uintptr_t op_eq_jumpback_a = 0,
			  op_eq_jumpback_b = 0,
			  op_eq_jumpback_c = 0,
			  op_eq_start = 0;

	op_eq_start = findConditionalStart(OP_EQ_HOOK);
	op_eq_jumpback_a = findConditionalJumpback(OP_EQ_HOOK) - 2;

	/* get destination of conditional */
	op_eq_jumpback_b = Mv2->Disassemble(op_eq_start + 4);

	/* jump back to original code */
	op_eq_jumpback_c = op_eq_start + 9;

	/* copy original code */
	sz_cloned_bytes = 3;

	std::memcpy(reinterpret_cast<char*>(vm_hooks),
		reinterpret_cast<void*>(op_eq_start), sz_cloned_bytes);

	/* recreate conditional */
	Mv2->Ja((uintptr_t)vm_hooks + sz_cloned_bytes,
		// ja op_eq_jumpback_b(sz: 6)
		op_eq_jumpback_b, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	/* check if unencrypted */
	Mv2->write_memory((uintptr_t)vm_hooks + sz_cloned_bytes,
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	Mv2->Je((uintptr_t)vm_hooks + sz_cloned_bytes,
		// je op_eq_jumpback_a (sz: 6)
		op_eq_jumpback_a, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	Mv2->Jmp((uintptr_t)vm_hooks + sz_cloned_bytes,
		// jmp op_eq_jumpback_c (sz: 5)
		op_eq_jumpback_c, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	MH_CreateHook(reinterpret_cast<void*>(op_eq_start),
		vm_hooks, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(op_eq_start)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}

	vm_hooks = (char*)vm_hooks + sz_cloned_bytes;

	uintptr_t op_ts_jumpback_a = 0,
			  op_ts_jumpback_b = 0,
			  op_ts_jumpback_c = 0,
			  op_ts_start = 0;

	op_ts_start		 = findConditionalStart(OP_TESTSET_HOOK);
	//op_ts_jumpback_a = findConditionalJumpback(OP_TESTSET_HOOK) - 5;
	op_ts_jumpback_a = op_lt_jumpback_a; // (same local)

	/* get destination of conditional */
	op_ts_jumpback_b = Mv2->Disassemble(op_ts_start + 4);

	/* jump back to original code */
	op_ts_jumpback_c = op_ts_start + 9;

	/* copy original code */
	sz_cloned_bytes = 3;

	std::memcpy(reinterpret_cast<char*>(vm_hooks),
		reinterpret_cast<void*>(op_ts_start), sz_cloned_bytes);

	/* recreate conditional */
	Mv2->Ja((uintptr_t)vm_hooks + sz_cloned_bytes,
		// ja op_ts_jumpback_b(sz: 6)
		op_ts_jumpback_b, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	/* check if unencrypted */
	Mv2->write_memory((uintptr_t)vm_hooks + sz_cloned_bytes,
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	Mv2->Je((uintptr_t)vm_hooks + sz_cloned_bytes,
		// je op_ts_jumpback_a (sz: 6)
		op_ts_jumpback_a, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	Mv2->Jmp((uintptr_t)vm_hooks + sz_cloned_bytes,
		// jmp op_ts_jumpback_c (sz: 5)
		op_ts_jumpback_c, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	MH_CreateHook(reinterpret_cast<void*>(op_ts_start),
		vm_hooks, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(op_ts_start)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}

	vm_hooks = (char*)vm_hooks + sz_cloned_bytes;

	uintptr_t op_test_jumpback_a = 0,
			  op_test_jumpback_b = 0,
			  op_test_jumpback_c = 0,
			  op_test_start = 0;

	op_test_start = findConditionalStart(OP_TEST_HOOK);
	op_test_jumpback_a = findConditionalJumpback(OP_TEST_HOOK) - 2;

	/* get destination of conditional */
	op_test_jumpback_b = Mv2->Disassemble(op_test_start + 4);

	/* jump back to original code */
	op_test_jumpback_c = op_test_start + 9;

	/* copy original code */
	sz_cloned_bytes = 3;

	std::memcpy(reinterpret_cast<char*>(vm_hooks),
				reinterpret_cast<void*>(op_test_start),
				sz_cloned_bytes);

	/* recreate conditional */
	Mv2->Ja((uintptr_t)vm_hooks + sz_cloned_bytes,
		// ja op_test_jumpback_b(sz: 6)
		op_test_jumpback_b, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	/* check if unencrypted */
	Mv2->write_memory((uintptr_t)vm_hooks + sz_cloned_bytes,
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	Mv2->Je((uintptr_t)vm_hooks + sz_cloned_bytes,
		// je op_test_jumpback_a (sz: 6)
		op_test_jumpback_a, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	Mv2->Jmp((uintptr_t)vm_hooks + sz_cloned_bytes,
		// jmp op_test_jumpback_c (sz: 5)
		op_test_jumpback_c, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	MH_CreateHook(reinterpret_cast<void*>(op_test_start),
		vm_hooks, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(op_test_start)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}

	vm_hooks = (char*)vm_hooks + sz_cloned_bytes;

	uintptr_t op_tforloop_jumpback_a = 0,
			  op_tforloop_jumpback_b = 0,
			  op_tforloop_jumpback_c = 0,
			  op_tforloop_start = 0;

	op_tforloop_start = findConditionalStart(OP_TFORLOOP_HOOK);
	op_tforloop_jumpback_a = findConditionalJumpback(OP_TFORLOOP_HOOK) - 2;

	/* get destination of conditional */
	op_tforloop_jumpback_b = Mv2->Disassemble(op_tforloop_start + 4);

	/* jump back to original code */
	op_tforloop_jumpback_c = op_tforloop_start + 9;

	/* copy original code */
	sz_cloned_bytes = 3;

	std::memcpy(reinterpret_cast<char*>(vm_hooks),
		reinterpret_cast<void*>(op_tforloop_start),
		sz_cloned_bytes);

	/* recreate conditional */
	Mv2->Ja((uintptr_t)vm_hooks + sz_cloned_bytes,
		// ja op_tforloop_jumpback_b(sz: 6)
		op_tforloop_jumpback_b, 
		(char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	/* check if unencrypted */
	Mv2->write_memory((uintptr_t)vm_hooks + sz_cloned_bytes,
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	Mv2->Je((uintptr_t)vm_hooks + sz_cloned_bytes,
		// je op_test_jumpback_a (sz: 6)
		op_tforloop_jumpback_a, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	Mv2->Jmp((uintptr_t)vm_hooks + sz_cloned_bytes,
		// jmp op_test_jumpback_c (sz: 5)
		op_tforloop_jumpback_c, (char*)vm_hooks + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	MH_CreateHook(reinterpret_cast<void*>(op_tforloop_start),
		vm_hooks, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(op_tforloop_start)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}
}

void PatchOpJmp(void *op_jmp_hook)
{
	uintptr_t op_jmp_jumpback = 0,
			  op_jmp_start = 0;

	op_jmp_start = findOpJmpStart(OP_JMP_HOOK);

	/* preprend original code to hook */
	size_t sz_cloned_bytes = 5;

	std::memcpy(reinterpret_cast<char*>(op_jmp_hook),
		reinterpret_cast<void*>(OP_JMP_HOOK), sz_cloned_bytes);

	/* check for encryption */
	Mv2->write_memory((uintptr_t)op_jmp_hook + sz_cloned_bytes,
		// cmp dword ptr [ebp-30], 1 (sz: 7)
		"\x81\x7D\xD0\x01\x00\x00\x00");

	sz_cloned_bytes += 7;

	op_jmp_jumpback = OP_JMP_HOOK + (op_jmp_start - OP_JMP_HOOK);

	Mv2->Je((uintptr_t)op_jmp_hook + sz_cloned_bytes,
		// je op_jmp_jumpback (sz: 6)
		op_jmp_jumpback, (char*)op_jmp_hook + sz_cloned_bytes);

	sz_cloned_bytes += 6;

	Mv2->Jmp((uintptr_t)op_jmp_hook + sz_cloned_bytes,
		// jmp op_jmp_orig (sz: 5)
		OP_JMP_HOOK + 5, (char*)op_jmp_hook + sz_cloned_bytes);

	sz_cloned_bytes += 5;

	// Hook OP_JMP to skip encryption

	MH_CreateHook(reinterpret_cast<void*>
		(OP_JMP_HOOK), op_jmp_hook, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>
		(OP_JMP_HOOK)) != MH_OK) {
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}
}

void LuaLoad::HookVM()
{
	MH_CreateHook(reinterpret_cast<void*>(vm_hook), &vm_hook_src, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>(vm_hook)) != MH_OK)
	{
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}

	VMJumpBack = vm_hook + 5;
	Mv2->write_memory(VMJumpBack, "\x90\x90");

	// allocate memory for our hooks
	char *vm_hooks = (char*)VirtualAlloc(nullptr, 2048, MEM_COMMIT,
		PAGE_EXECUTE_READWRITE
	);

	int *vm_table = findVMSwitchTable(vm_hook);

	// fetch instructions from switch table

	if (vm_table)
	{
		OP_MOVE_HOOK = vm_table[0x6];
		OP_RETURN_HOOK = vm_table[0x22];
		OP_CLOSURE_HOOK = vm_table[0x25];
		OP_SETUPVAL_HOOK = vm_table[0x5];
		OP_CALL_HOOK = vm_table[0x1f];
		OP_JMP_HOOK = vm_table[0x14];

		PatchOpMove();
		PatchOpClose();

		PatchOpRet(vm_hooks);
		vm_hooks += 100;

		PatchOpCall(vm_hooks);
		vm_hooks += 100;

		PatchOpClosure(vm_hooks);
		vm_hooks += 100;

		PatchOpSetupval(vm_hooks);
		vm_hooks += 100;

		PatchOpJmp(vm_hooks);
		vm_hooks += 100;

		PatchConditionals(vm_table,
			vm_hooks);

		vm_hooks += 200;
	}
}

void __stdcall RunScript(void* code, size_t szDump)
{
	int g_L = getstateindex(0);

	int thread = r_lua_newthread(g_L);   //TODO set identity level

	std::string chunkName = "=" + random_string();

	LoadScriptT(thread, (std::string*)code, &chunkName[0], szDump);

	r_lua_resume(thread, 0);
	r_lua_pop(g_L, 1);
}

void __declspec(naked) ud_Hook() {

	__asm
	{
		push ebp;
		mov ebp, esp;
		mov eax, fs:[0];
		pushad
		push[ebp + 20];
		push[ebp + 16];
		push[ebp + 12];
		push[ebp + 8];
		call[ConvertToProto];
		cmp eax, -1;
		popad;
		je udorig;
		xor eax, eax
		mov esp, ebp;
		pop ebp;
		ret;
		udorig:
		jmp[LoadScriptJumpBack];
	}
}

void LuaLoad::HookUndump()
{

	MH_CreateHook(reinterpret_cast<void*>(LoadScriptT), &ud_Hook, nullptr);

	if (MH_EnableHook(reinterpret_cast<void*>(LoadScriptT)) != MH_OK)
	{
		std::cout << "Failed to enable hook!" << std::endl;
		return;
	}

	LoadScriptJumpBack = (uintptr_t)LoadScriptT + 5;

	Mv2->write_memory(LoadScriptJumpBack, "\x90\x90\x90\x90");
}

DWORD WINAPI RCSvnInit(LPVOID)
{

	HANDLE processHandle = Mv2->GetProcess();

	signature_scanner *newscan = new signature_scanner(processHandle, ModBaseAddr, 0xf00000);

	//CreateDString = (CreateDString_Def)newscan->search("558BEC538B5D08568BF18B4D0C578B43103BC10F82E90000008B");
	rluaDumpToProto = (rluaDumpToProto_Def)newscan->search("83ec34????8b??088d??????8d????c7", 0, true);
	r_luaM_realloc = (r_luaM_realloc_Def)newscan->search("83c41885c0750485", 0, true);
	LoadScriptT = (LoadScript_Def)newscan->search("81ecd80000008b4d0c8b411085c00f85", 0, true);
	r_luaS_newlstr = (r_luaS_newlstr_Def)newscan->search("8a??033a??0374??8b??088b??85", 0, true);
	r_lua_newthread = (r_lua_newthread_Def)newscan->search("72????e8????????83c404??e8????????8b", 0, true);
	r_lua_settop = (r_lua_settop_Def)newscan->search("c74008????????83????108b????03", 0, true);
	r_lua_resume = (r_lua_resume_Def)newscan->search("c3b8c8000000663946??72", 0, true);
	r_lua_gettop = (r_lua_gettop_Def)newscan->search("558bec8b??088b????2b????c1??045dc3");
	vm_hook = newscan->search("0faf55??89????89????89");

	uintptr_t getlstatefromindex = newscan->search("8d????00000000????8d??????000000????8b????????e8");

	uintptr_t proto_hash_check = newscan->search("09????8b????b8????????2b????f7");

	create_inline_func(getlstatefromindex);

	if (newscan->err())
	{
		MessageBox(NULL, "One or more results were not found",
						 "Critical Error!", MB_ICONWARNING | MB_TOPMOST);
		return 0;
	}

	Mv2->write_memory(proto_hash_check, "\x90\x90\x90");

	// std::cout << std::hex << LoadScriptT << "\n" << rluaDumpToProto << "\n";

	std::vector<uintptr_t> rluaRetChecks = {
		(uintptr_t)r_lua_newthread, (uintptr_t)r_lua_resume + 0x64, // pad to avoid first rets
		(uintptr_t)r_lua_settop
	};

	// ret check bypass
	for (const auto& _retcheck : rluaRetChecks)
	{
		auto stackchk = fetchRetCheckOffsets(_retcheck);

		for (size_t i = 0; i < stackchk.size(); i++)
		{
			if (stackchk[i])
			{
				Mv2->write_memory(_retcheck + stackchk[i], "\xEB");
			}
		}
	}

	delete newscan;

	LuaLoad::HookVM();
	LuaLoad::HookUndump();

	return 0;
}

uint32_t UlongToHexString(uint64_t num, char *s)
{
	uint64_t x = num;

	// use bitwise-ANDs and bit-shifts to isolate
	// each nibble into its own byte
	// also need to position relevant nibble/byte into
	// proper location for little-endian copy
	x = ((x & 0xFFFF) << 32) | ((x & 0xFFFF0000) >> 16);
	x = ((x & 0x0000FF000000FF00) >> 8) | (x & 0x000000FF000000FF) << 16;
	x = ((x & 0x00F000F000F000F0) >> 4) | (x & 0x000F000F000F000F) << 8;

	// Now isolated hex digit in each byte
	// Ex: 0x1234FACE => 0x0E0C0A0F04030201

	// Create bitmask of bytes containing alpha hex digits
	// - add 6 to each digit
	// - if the digit is a high alpha hex digit, then the addition
	//   will overflow to the high nibble of the byte
	// - shift the high nibble down to the low nibble and mask
	//   to create the relevant bitmask
	//
	// Using above example:
	// 0x0E0C0A0F04030201 + 0x0606060606060606 = 0x141210150a090807
	// >> 4 == 0x0141210150a09080 & 0x0101010101010101
	// == 0x0101010100000000
	//
	uint64_t mask = ((x + 0x0606060606060606) >> 4) & 0x0101010101010101;

	// convert to ASCII numeral characters
	x |= 0x3030303030303030;

	// if there are high hexadecimal characters, need to adjust
	// for uppercase alpha hex digits, need to add 0x07
	//   to move 0x3A-0x3F to 0x41-0x46 (A-F)
	// for lowercase alpha hex digits, need to add 0x27
	//   to move 0x3A-0x3F to 0x61-0x66 (a-f)
	// it's actually more expensive to test if mask non-null
	//   and then run the following stmt
	x += (0x7 * mask);

	//copy string to output buffer
	*(uint64_t *)s = x;

	return 0;
}

int rand_num(int min, int max)
{
	if (!max)
		max = 20;

	if (!min)
		min = 10;

	std::random_device seed;
	std::mt19937 gen{ seed() }; // seed the generator
	std::uniform_int_distribution<int> dist(min, max);

	int guess = dist(gen);

	return guess;
}

std::string random_string()
{
	size_t length = rand_num();

	auto randchar = []() -> char
	{
		const char charset[] =
			"0123456789"
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz";
		const size_t max_index = (sizeof(charset) - 1);
		return charset[rand_num(1, 60) % max_index];
	};
	std::string str(length, 0);
	std::generate_n(str.begin(), length, randchar);
	return str;
}

/*
int __stdcall ConvertToProto(int r_lua_State, std::string* code, const char* source, int unk)
{
	unsigned char luaMagic = *reinterpret_cast<unsigned char*>(code);// header

	// todo: run regular localscripts with no bytecode
	// todo: protect against descendantadded for coregui, and optional backpack/playergui

	DeserializedString ds;

	if (luaMagic != 0x10)
	{
		size_t szCompressed = *(uintptr_t*)(UCHAR*)&code + 16;

		szCompressed = *(uintptr_t*)(szCompressed);

		if (szCompressed > 8)
		{

			char *newCode;

			CreateDString(&ds, code, 0, -1);

			// Calculate hash

			uintptr_t xxhash = *ds.result;

			size_t _ptr = 0;

			do
			{
				*((BYTE *)&xxhash + _ptr) =
					(*((BYTE *)&xxhash + _ptr) ^ kBytecodeMagic[_ptr]) - 41 * _ptr;
				++_ptr;
			} while (_ptr < 4);

			_ptr = 0;

			// Decrypt dump

			if (ds.szlower)
			{
				do
				{
					newCode = (char*)&ds.result;
					if (ds.szhigher >= 0x10)
						newCode = (char*)ds.result;
					*((BYTE *)newCode + _ptr) ^= *((BYTE *)&xxhash + (_ptr & 3)) + 41 * _ptr;
					++_ptr;
				} while (_ptr < ds.szlower);

				// might need to decompress for bigger dumps

				const char* newSource = r_luaS_newlstr(r_lua_State,
					source,
					strlen(source)
				);

				lDumpInfo *ldi = new lDumpInfo;
				ldi->unk_1 = 1;
				ldi->unk_2 = 15;

				lDumpMetaData md = { 0 };
				md.source = newCode + 0x11;
				md.eof = md.source + newCode[4];
				md._eof = md.eof;

				ldi->md = &md;

				int *proto = rluaDumpToProto((int)ldi, r_lua_State, newSource, 1);

				int env = *(int*)(r_lua_State + 64);
				r_LClosure* nlc = r_luaF_newLclosure(r_lua_State, 0, env);
				encrypt_ptr((int)&nlc->p, proto);
				r_lua_pushLclosure(r_lua_State, nlc);

				// printf("%x%s does not belong to rc7\n", luaMagic, source);

				delete ldi;
				return 0;//return -1;
			}
		}
	}

	return -1;
}
*/
