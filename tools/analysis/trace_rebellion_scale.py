"""Read-only Rebellion render-scale consumer trace using the existing PE/Capstone approach.

Requires pefile and capstone. Inspects an EXE as data; never loads it, attaches to
a process, writes game memory, or calls a discovered function. Addresses are
derived from setting names and instruction relationships, not an executable hash.
"""
import argparse
import bisect
import hashlib
import json
import ntpath
import re
import struct

import capstone as cs
import pefile
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_RIP


class Image:
    def __init__(self, path):
        self.path = path
        self.pe = pefile.PE(path, fast_load=True)
        directory = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        records = list(struct.iter_unpack('<III', self.pe.get_data(directory.VirtualAddress, directory.Size)))
        self.unwind = {a: u for a, b, u in records if a < b}
        self.functions = sorted((a, b) for a, b, _ in records if a < b)
        self.starts = [a for a, _ in self.functions]
        self.decoder = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
        self.decoder.detail = True
        self.cache = {}

    def owner(self, address):
        index = bisect.bisect_right(self.starts, address) - 1
        if index < 0 or address >= self.functions[index][1]:
            raise ValueError(f'No unwind range for {address:x}')
        return self.functions[index]

    def instructions(self, address, through_return=False):
        start, end = self.owner(address)
        if through_return:
            start, end = address, address + 2048
        key = start, end
        if key not in self.cache:
            result = []
            for instruction in self.decoder.disasm(self.pe.get_data(start, end-start), start):
                result.append(instruction)
                if through_return and instruction.mnemonic == 'ret':
                    break
            self.cache[key] = result
        return self.cache[key]

    def function_start(self, address):
        """Resolve Microsoft's chained unwind fragments to the primary entry."""
        start, _ = self.owner(address)
        seen = set()
        unwind = self.unwind[start]
        while unwind not in seen:
            seen.add(unwind)
            header = self.pe.get_data(unwind, 4)
            if not (header[0] >> 3) & 4:  # UNW_FLAG_CHAININFO
                return start
            tail = unwind + 4 + ((header[2] + 1) & ~1) * 2
            start, _, unwind = struct.unpack('<III', self.pe.get_data(tail, 12))
        raise ValueError('Cyclic chained unwind metadata')

    @staticmethod
    def memory(instruction, operand):
        if operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
            return instruction.address + instruction.size + operand.mem.disp
        return None

    def refs(self, target):
        emitted = set()
        # Include RIP instructions with a one/four-byte immediate after disp32.
        endings = {target, target-1, target-4}
        for section in self.pe.sections:
            if not section.Characteristics & 0x20000000:
                continue
            data, origin = section.get_data(), section.VirtualAddress
            for match in re.finditer(rb'(?=[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]....)', data, re.S):
                at = match.start()
                displacement = int.from_bytes(data[at+1:at+5], 'little', signed=True)
                if origin+at+5+displacement not in endings:
                    continue
                try:
                    instructions = self.instructions(origin+at)
                except ValueError:
                    continue
                for instruction in instructions:
                    if instruction.address <= origin+at < instruction.address+instruction.size:
                        if instruction.address not in emitted and any(
                                self.memory(instruction, operand) == target for operand in instruction.operands):
                            emitted.add(instruction.address)
                            yield instruction
                        break

    def string(self, value, encoding):
        needle, results, start = (value+'\0').encode(encoding), [], 0
        while (offset := self.pe.__data__.find(needle, start)) >= 0:
            results.append(self.pe.get_rva_from_offset(offset))
            start = offset+1
        if len(results) != 1:
            raise ValueError(f'{value}: expected one {encoding} anchor, found {len(results)}')
        return results[0]

    def float(self, rva):
        return struct.unpack('<f', self.pe.get_data(rva, 4))[0]


def unique(values, description):
    values = sorted(set(values))
    if len(values) != 1:
        raise ValueError(f'{description}: expected one relationship, found {len(values)}')
    return values[0]


def dimension_chain(image, body, scale_register):
    """Recognize the two actual multiply->integer->stored dimension chains.

    This is a straight-line instruction relationship check, not a general CFG
    solver or proof that the code executed. Both accepted compiler shapes are
    independently inspected in the accompanying evidence receipt.
    """
    registers, floats, dimensions, scales = {}, {}, [], []
    def register(ins, operand):
        name = ins.reg_name(operand.reg)
        aliases = {'eax':'rax','ebx':'rbx','ecx':'rcx','edx':'rdx',
                   'esi':'rsi','edi':'rdi','ebp':'rbp','esp':'rsp'}
        if re.fullmatch(r'r(?:[89]|1[0-5])d', name):
            return name[:-1]
        return aliases.get(name, name)
    for ins in body:
        operands = ins.operands
        if len(operands) != 2:
            continue
        a,b = operands
        if (ins.mnemonic == 'movss' and image.memory(ins,a) is not None
                and b.type == X86_OP_REG and register(ins,b) == scale_register):
            scales.append(image.memory(ins,a))
        if ins.mnemonic == 'mov' and a.type == X86_OP_REG:
            address = image.memory(ins,b)
            registers[register(ins,a)] = ('dimension',address) if address is not None else None
        elif ins.mnemonic == 'cvtsi2ss' and a.type == X86_OP_REG and b.type == X86_OP_REG:
            floats[register(ins,a)] = registers.get(register(ins,b))
        elif ins.mnemonic == 'mulss' and b.type == X86_OP_REG and register(ins,b) == scale_register:
            value = floats.get(register(ins,a))
            floats[register(ins,a)] = ('scaled',value[1]) if value and value[0] == 'dimension' else None
        elif ins.mnemonic == 'cvttss2si' and a.type == X86_OP_REG and b.type == X86_OP_REG:
            registers[register(ins,a)] = floats.get(register(ins,b))
        elif ins.mnemonic == 'mov' and a.type == X86_OP_MEM and b.type == X86_OP_REG:
            value, destination = registers.get(register(ins,b)), image.memory(ins,a)
            if value and value[0] == 'scaled' and destination is not None:
                dimensions.append((value[1],destination,ins.address))
    if len(dimensions) != 2 or len(set(scales)) != 1:
        return None
    following = [ins for ins in body if ins.address > dimensions[-1][2] and ins.mnemonic == 'call'
                 and ins.operands[0].type == X86_OP_IMM]
    if not following:
        return None
    return scales[0], dimensions, following[0].operands[0].imm


def trace(path):
    image = Image(path)
    try:
        return trace_image(image)
    finally:
        image.pe.close()


def trace_image(image):
    ui = image.string('Frontend.Options.Display.RenderScale', 'utf-16-le')
    parser = image.string('RenderScale', 'ascii')
    settings = []
    for reference in image.refs(ui):
        instructions = image.instructions(reference.address)
        index = next(i for i, ins in enumerate(instructions) if ins.address == reference.address)
        preceding = [ins for ins in instructions[max(0,index-6):index]
                     if ins.mnemonic == 'lea' and ins.reg_name(ins.operands[0].reg) == 'rax'
                     and image.memory(ins, ins.operands[1]) is not None]
        if preceding:
            settings.append(image.memory(preceding[-1], preceding[-1].operands[1]))
    setting = unique(settings, 'UI setting pointer')

    # The parser must write this SAME setting, and its scale quantizer must be
    # demonstrated by instructions/constants rather than a numeric memory hit.
    quantizers = []
    for reference in image.refs(parser):
        instructions = image.instructions(reference.address)
        for index, ins in enumerate(instructions):
            if ins.mnemonic != 'movss' or image.memory(ins, ins.operands[0]) != setting:
                continue
            region = instructions[max(0,index-32):index]
            multipliers = [image.float(image.memory(step,step.operands[1])) for step in region
                           if step.mnemonic == 'mulss' and image.memory(step,step.operands[1]) is not None]
            if len(multipliers) == 2 and abs(multipliers[0]*multipliers[1]-1) < 1e-6:
                quantizers.append((image.owner(reference.address)[0], multipliers[0], multipliers[1]))
    parser_function, quantizer_multiplier, quantizer_step = unique(quantizers, 'parsed setting quantizer')

    # Follow the pending-setting mirror back to its separate applied cache.
    # SE5 calls a helper; Resistance inlines the same operation in settings apply.
    applies = []
    for reference in image.refs(setting):
        if reference.mnemonic != 'movss' or image.memory(reference,reference.operands[0]) != setting:
            continue
        instructions = image.instructions(reference.address)
        index = next(i for i, ins in enumerate(instructions) if ins.address == reference.address)
        mirror_register = reference.reg_name(reference.operands[1].reg)
        cache_reads = [ins for ins in instructions[max(0,index-4):index] if ins.mnemonic == 'movss'
                       and ins.reg_name(ins.operands[0].reg) == mirror_register
                       and image.memory(ins,ins.operands[1]) is not None]
        if not cache_reads:
            continue
        cache = image.memory(cache_reads[-1],cache_reads[-1].operands[1])
        stores = [(i, ins) for i, ins in enumerate(instructions[:index]) if ins.mnemonic == 'movss'
                  and image.memory(ins,ins.operands[0]) == cache]
        for store_index, store in stores:
            # Inlined shape: retain the source register from the cache assignment.
            scale_register = store.reg_name(store.operands[1].reg)
            inline_body = instructions[store_index+1:index]
            chain = dimension_chain(image, inline_body, scale_register)
            if chain:
                applies.append((image.function_start(reference.address), None, store.address, cache, chain))
                continue
            # Out-of-line shape: the helper must independently read that cache
            # and perform both dimension calculations before any result is used.
            for ins in instructions[store_index+1:index]:
                if ins.mnemonic != 'call' or ins.operands[0].type != X86_OP_IMM:
                    continue
                target = ins.operands[0].imm
                try:
                    body = image.instructions(target, through_return=True)
                except ValueError:
                    continue
                if not body or body[-1].mnemonic != 'ret':
                    continue
                for load in body:
                    if load.mnemonic == 'movss' and image.memory(load,load.operands[1]) == cache:
                        chain = dimension_chain(image, body, load.reg_name(load.operands[0].reg))
                        if chain:
                            applies.append((image.function_start(reference.address), target, ins.address, cache, chain))
    if len(applies) != 1:
        raise ValueError(f'settings-to-native-apply link: expected one relationship, found {len(applies)}')
    settings_apply, native_apply, apply_site, applied_setting, chain = applies[0]
    active_scale, dimensions, resource_update = chain
    return dict(image=image.path,sha256=hashlib.sha256(image.pe.__data__).hexdigest(),evidence='static executable dataflow; not a live call receipt',
                ui_anchor=ui,parser_function=parser_function,setting=setting,quantizer_multiplier=quantizer_multiplier,
                quantizer_step=quantizer_step,settings_apply=settings_apply,applied_setting=applied_setting,
                native_apply=native_apply,apply_site=apply_site,apply_shape='call' if native_apply else 'inline',
                active_scale=active_scale,
                full_width=dimensions[0][0],scene_width=dimensions[0][1],full_height=dimensions[1][0],
                scene_height=dimensions[1][1],resource_update=resource_update)


def watchpoints(result):
    """Prepare standard debugger commands, never attach or execute them.

    Four one-shot processor access breakpoints capture pending-setting access,
    applied-setting writes and both committed dimensions. Module-relative names
    handle ASLR. They do not patch instructions or issue settings/function calls.
    Use an otherwise empty hardware-breakpoint session, during a native menu
    scale change. This is diagnostic evidence, never a performance measurement.
    """
    module = ntpath.splitext(ntpath.basename(result['image']))[0]
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*',module):
        raise ValueError('Unsupported debugger module name')
    def address(name):
        return f'{module}+0x{result[name]:x}'
    snapshot = '; '.join(f'.echo {name}; {"df" if "setting" in name or name == "active_scale" else "dd"} {address(name)} L1'
                         for name in ('setting','applied_setting','active_scale','full_width','full_height','scene_width','scene_height'))
    lines = ['$$ Generated read-only trace setup, not a controller or attach script.',
             '$$ Exact inspected image SHA256: '+result['sha256'],
             '$$ Use the same inspected executable in a clean hardware-breakpoint session.',
             '$$ Enable debugger logging, load this file, then resume and change native render scale.',
             '$$ One-shot events record real thread/stack/values and automatically continue.',
             '$$ No game settings, memory values or native functions are changed by these commands.']
    for name, access in (('setting','r'),('applied_setting','w'),('scene_width','w'),('scene_height','w')):
        commands = f'.echo NA_RENDER_SCALE_{name}; ~.; ub @rip L4; k 8; {snapshot}; gc'
        lines.append(f'ba {access} 4 /1 {address(name)} "{commands}"')
    lines.append('bl')
    return '\n'.join(lines)+'\n'


if __name__ == '__main__':
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument('image')
    arguments.add_argument('--json', dest='output')
    arguments.add_argument('--watchpoints', help='Write optional one-shot WinDbg commands; never execute/attach')
    options = arguments.parse_args()
    result = trace(options.image)
    text = json.dumps(result,indent=2)
    if options.output:
        with open(options.output,'w',encoding='utf-8') as output:
            output.write(text+'\n')
    if options.watchpoints:
        with open(options.watchpoints,'w',encoding='utf-8') as output:
            output.write(watchpoints(result))
    print(text)
