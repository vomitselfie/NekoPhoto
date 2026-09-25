# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp>=1.10,<2"]
# ///
"""Smoke test for the MCP bridge: runs mcp/nekophoto_mcp.py over stdio with the MCP client, the way an
agent's client does, against an editor already listening (CI starts one headless with --demo first):

    nekophoto --headless --rpc-socket /tmp/c.sock --demo &
    python3 tools/mcp_smoke.py /tmp/c.sock        # or: uv run tools/mcp_smoke.py /tmp/c.sock

Checks that every tool carries a title and annotations, the prompt renders, and a short look, edit, verify,
undo session works, with images coming back as MCP image content and errors as tool errors.
"""
import asyncio
import os
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

BRIDGE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "mcp", "nekophoto_mcp.py")


def texts(result):
    return "\n".join(c.text for c in result.content if c.type == "text")


async def main(sock: str) -> None:
    env = dict(os.environ, COMPOSITOR_RPC_SOCKET=sock, COMPOSITOR_MCP_LAUNCH="0")
    server = StdioServerParameters(command=sys.executable, args=[BRIDGE], env=env)
    async with stdio_client(server) as (read, write), ClientSession(read, write) as session:
        init = await session.initialize()
        assert "document_overview" in (init.instructions or ""), init.instructions

        tools = (await session.list_tools()).tools
        names = {t.name for t in tools}
        for t in tools:
            assert t.title, f"{t.name} has no title"
            assert t.annotations and t.annotations.readOnlyHint is not None, f"{t.name} has no annotations"
        for needed in ("document_overview", "describe_method", "render", "layers_set", "rpc", "tool_select", "layers_get"):
            assert needed in names, needed
        reading = {t.name for t in tools if t.annotations.readOnlyHint}
        assert {"render", "document_overview", "layers_list"} <= reading, reading
        assert not {"layers_set", "document_export", "rpc"} & reading

        prompts = (await session.list_prompts()).prompts
        assert any(p.name == "edit_photo" for p in prompts), prompts
        prompt = await session.get_prompt("edit_photo", {"goal": "brighten the sky"})
        assert "brighten the sky" in prompt.messages[0].content.text

        # Look.
        overview = await session.call_tool("document_overview", {"render": True, "max_size": 256})
        assert not overview.isError, texts(overview)
        assert "Layers, top first" in texts(overview), texts(overview)
        images = [c for c in overview.content if c.type == "image"]
        assert images and images[0].mimeType == "image/png", overview.content
        described = await session.call_tool("describe_method", {"method": "layers.set"})
        assert "Color Dodge" in texts(described), texts(described)

        # Act, verify, undo.
        added = await session.call_tool("layers_add", {"kind": "pixels", "name": "MCP smoke"})
        assert not added.isError, texts(added)
        filled = await session.call_tool("pixels_fill", {"color": "#3366ff"})
        assert not filled.isError, texts(filled)
        render = await session.call_tool("render", {"max_size": 128})
        assert render.content[0].type == "image", render.content
        zoomed = await session.call_tool("render", {"x": 0, "y": 0, "width": 20, "height": 20, "zoom": 4})
        assert not zoomed.isError and zoomed.content[0].type == "image", texts(zoomed)
        assert "MCP smoke" in texts(await session.call_tool("document_overview", {}))
        undone = await session.call_tool("history_undo", {"steps": 2})
        assert not undone.isError, texts(undone)
        assert "MCP smoke" not in texts(await session.call_tool("document_overview", {}))

        # An undo group, and a named batch that fails part-way and is taken back.
        await session.call_tool("history_group_begin", {"name": "MCP group"})
        await session.call_tool("layers_add", {"kind": "pixels", "name": "G1"})
        await session.call_tool("pixels_fill", {"color": "#ff0000"})
        ended = await session.call_tool("history_group_end", {})
        assert '"merged": 3' in texts(ended), texts(ended)
        await session.call_tool("history_undo", {})
        assert "G1" not in texts(await session.call_tool("document_overview", {}))
        batch = await session.call_tool("batch", {"calls": [{"method": "render", "params": {"maxSize": 64}},
                                                            {"method": "document.overview", "params": {}}]})
        assert not batch.isError and [c.type for c in batch.content] == ["text", "image"], batch.content
        failed = await session.call_tool("batch", {"name": "All or nothing", "calls": [
            {"method": "layers.add", "params": {"kind": "pixels", "name": "Rolled back layer"}},
            {"method": "layers.set", "params": {"id": "nope"}}]})
        assert failed.isError and "taken back" in texts(failed), texts(failed)
        assert "Rolled back layer" not in texts(await session.call_tool("document_overview", {}))

        # Errors come back as tool errors that say what to do.
        bad = await session.call_tool("rpc", {"method": "layers.set", "params": {"id": "x", "opacty": 1}})
        assert bad.isError and "takes id, name" in texts(bad), texts(bad)
        missing = await session.call_tool("layers_get", {"id": "nope"})
        assert missing.isError and "document.overview" in texts(missing), texts(missing)

    print(f"{len(tools)} tools, {len(prompts)} prompt; MCP smoke test passed")


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("COMPOSITOR_RPC_SOCKET", "")))
