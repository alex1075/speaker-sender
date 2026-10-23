import asyncio, pyatv

async def main():
    loop = asyncio.get_running_loop()
    for d in await pyatv.scan(loop, timeout=8):
        svcs = ", ".join(f"{s.protocol.name}:{s.port}{'(pw)' if s.requires_password else ''}" for s in d.services)
        print(f"{d.name!r:35} {d.address!s:16} id={d.identifier} model={d.device_info.model.name} os={d.device_info.operating_system.name} ver={d.device_info.version}\n    {svcs}")

asyncio.run(main())
