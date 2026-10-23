import asyncio, sys, pyatv
from pyatv.const import Protocol

async def main(name):
    loop = asyncio.get_running_loop()
    for d in await pyatv.scan(loop, timeout=8):
        if d.name == name:
            for p in (Protocol.RAOP, Protocol.AirPlay):
                s = d.get_service(p)
                if s:
                    print(p.name, s.port)
                    for k, v in sorted(s.properties.items()):
                        print(f"   {k} = {v}")

asyncio.run(main(sys.argv[1]))
