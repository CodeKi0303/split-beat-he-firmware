"""Actual stdio subprocess initialize/list/call through official MCP SDK."""
import asyncio
import pathlib
import sys

from mcp import Client
from mcp.client.stdio import StdioServerParameters

ROOT=pathlib.Path(__file__).resolve().parents[1]
LEGACY_TOOLS = {
    'keyboard_info', 'keyboard_status', 'keyboard_get_config',
    'keyboard_export_profile', 'keyboard_validate_profile', 'keyboard_set_key',
    'keyboard_set_binding', 'keyboard_calibrate', 'keyboard_output',
    'keyboard_import_profile', 'keyboard_save',
}
SESSION_DIAGNOSTIC_TOOLS = {
    'keyboard_list_devices', 'keyboard_session', 'keyboard_connect',
    'keyboard_disconnect', 'keyboard_health', 'keyboard_capture_sensors',
    'keyboard_measure_performance', 'keyboard_measure_link',
    'keyboard_adc_trace', 'keyboard_cpu_profile', 'keyboard_adc_irq_profile',
    'keyboard_interference_baseline', 'keyboard_interference_compare',
}


async def run(write, mode):
    args=['-m','host.mcp_server','--demo']+(['--write-enabled'] if write else [])
    params=StdioServerParameters(command=sys.executable,args=args,cwd=ROOT)
    async with Client(params, mode=mode) as client:
        tools=await client.list_tools()
        names={tool.name for tool in tools.tools}
        assert LEGACY_TOOLS | SESSION_DIAGNOSTIC_TOOLS <= names,names
        assert 'keyboard_curve_catalog' in names
        curves=await client.call_tool('keyboard_curve_catalog',{})
        assert not curves.is_error,curves
        assert curves.structured_content['curves'][1]['recommended_total_travel_mm']==3.4
        info=await client.call_tool('keyboard_info',{})
        assert not info.is_error,info
        assert info.structured_content['demo'] is True
        trace=await client.call_tool('keyboard_adc_trace',{'keys':[5,6]})
        assert not trace.is_error,trace
        assert trace.structured_content['supported'] is False,trace
        assert trace.structured_content['demo'] is True,trace
        assert trace.structured_content['changed_device_settings'] is False,trace
        assert trace.structured_content['changed_output'] is False,trace
        cpu=await client.call_tool('keyboard_cpu_profile',{})
        assert not cpu.is_error,cpu
        assert cpu.structured_content['supported'] is False,cpu
        assert cpu.structured_content['demo'] is True,cpu
        assert cpu.structured_content['changed_output'] is False,cpu
        irq=await client.call_tool('keyboard_adc_irq_profile',{})
        assert not irq.is_error,irq
        assert irq.structured_content['supported'] is False,irq
        assert irq.structured_content['demo'] is True,irq
        assert irq.structured_content['changed_output'] is False,irq
        for keys in ([5,5],[True,6],[5]):
            invalid_trace=await client.call_tool('keyboard_adc_trace',{'keys':keys})
            assert invalid_trace.is_error,invalid_trace
        status=await client.call_tool('keyboard_status',{'side':1})
        assert len(status.structured_content['raw'])==7
        config=await client.call_tool('keyboard_get_config',{})
        assert len(config.structured_content['keys'])==14
        result=await client.call_tool('keyboard_set_binding',{'input':0,'kind':1,'code':44})
        assert bool(result.is_error)==(not write),result
        chord={'input':1,'kind':6,'keys':[4,5,6],'modifiers':2}
        result=await client.call_tool('keyboard_set_binding',chord)
        assert bool(result.is_error)==(not write),result
        if write:
            assert result.structured_content==chord,result
        invalid_type=await client.call_tool('keyboard_status',{'side':True})
        assert invalid_type.is_error
        health=await client.call_tool('keyboard_health',{})
        assert not health.is_error,health
        assert health.structured_content['info']['demo'] is True
        capture=await client.call_tool('keyboard_capture_sensors',{
            'duration_seconds':.2,'sample_hz':10,'include_samples':True})
        assert not capture.is_error,capture
        assert len(capture.structured_content['statistics'])==14
        assert 2 <= len(capture.structured_content['samples']) <= 3
        bad_capture=await client.call_tool('keyboard_capture_sensors',{
            'duration_seconds':11.0})
        assert bad_capture.is_error,bad_capture
        for name in ('keyboard_measure_performance','keyboard_measure_link'):
            measured=await client.call_tool(name,{'duration_seconds':.2})
            assert not measured.is_error,measured
            assert measured.structured_content['supported'] is False
            assert measured.structured_content['demo'] is True
        compare=await client.call_tool('keyboard_interference_compare',{
            'key':0,'duration_seconds':.2})
        assert compare.is_error,compare
        if write:
            # The optional field is capability-checked by the client, and old
            # tool callers must preserve a configured lower deadzone.
            args=dict(key=0,enabled=True,rapid_trigger=True,actuation=200,
                      rt_press=100,rt_release=100,top_deadzone=20,noise_deadband=5)
            lower=await client.call_tool('keyboard_set_key',{**args,'bottom_deadzone':125,'curve_id':1})
            assert not lower.is_error and lower.structured_content['bottom_deadzone']==125,lower
            assert lower.structured_content['curve_id']==1,lower
            retained=await client.call_tool('keyboard_set_key',args)
            assert not retained.is_error and retained.structured_content['bottom_deadzone']==125,retained
            assert retained.structured_content['curve_id']==1,retained
            invalid_curve=await client.call_tool('keyboard_set_key',{**args,'curve_id':True})
            assert invalid_curve.is_error,invalid_curve
            save=await client.call_tool('keyboard_save',{})
            assert not save.is_error,save
            assert save.structured_content['verified'] is True
            invalid=await client.call_tool('keyboard_import_profile',{'profile':{'schema':'bad'}})
            assert invalid.is_error
            released=await client.call_tool('keyboard_demo_pose',{
                'keys':list(range(14)),'value':0})
            assert not released.is_error,released
            started=await client.call_tool('keyboard_batch_calibration_start',{'press_seconds':10})
            assert not started.is_error,started
            assert started.structured_content['state']=='baseline'
            blocked=await client.call_tool('keyboard_set_binding',{'input':0,'kind':1,'code':4})
            assert blocked.is_error,blocked
            await asyncio.sleep(3.2)
            progress=await client.call_tool('keyboard_batch_calibration_status',{})
            assert not progress.is_error,progress
            assert progress.structured_content['state']=='press',progress
            pressed=await client.call_tool('keyboard_demo_pose',{'keys':[0,7],'value':1000})
            assert not pressed.is_error,pressed
            await asyncio.sleep(.15)
            progress=await client.call_tool('keyboard_batch_calibration_status',{})
            assert progress.structured_content['eligible_keys']==[0,7],progress
            applied=await client.call_tool('keyboard_batch_calibration_apply',{'keys':[0,7]})
            assert not applied.is_error,applied
            assert applied.structured_content['applied_ram'] is True,applied
            assert applied.structured_content['persisted'] is False
            assert applied.structured_content['applied_keys']==[0,7]
            post=await client.call_tool('keyboard_get_config',{})
            assert post.structured_content['keys'][0]['bottom_deadzone']==125
            assert post.structured_content['keys'][0]['curve_id']==1
            assert post.structured_content['keys'][1]['flags'] & 4 == 0
            stopped=await client.call_tool('keyboard_batch_calibration_cancel',{})
            assert not stopped.is_error,stopped
            released=await client.call_tool('keyboard_demo_pose',{'keys':[0,7],'value':0})
            baseline=await client.call_tool('keyboard_interference_baseline',{'duration_seconds':.2})
            assert not baseline.is_error,baseline
            await client.call_tool('keyboard_demo_pose',{'keys':[0],'value':1000})
            compared=await client.call_tool('keyboard_interference_compare',{'key':0,'duration_seconds':.2})
            assert not compared.is_error,compared
            assert compared.structured_content['sufficient_selected_motion'] is True
            assert len(compared.structured_content['other_keys'])==13
        print('stdio initialize/list/call',mode,'write-enabled' if write else 'read-only','PASS')


async def disconnected_startup():
    params=StdioServerParameters(command=sys.executable,
        args=['-m','host.mcp_server'],cwd=ROOT)
    async with Client(params) as client:
        state=await client.call_tool('keyboard_session',{})
        assert not state.is_error and state.structured_content['connected'] is False,state
        missing=await client.call_tool('keyboard_info',{})
        assert missing.is_error,missing
        missing_trace=await client.call_tool('keyboard_adc_trace',{'keys':[5,6]})
        assert missing_trace.is_error,missing_trace
        missing_cpu=await client.call_tool('keyboard_cpu_profile',{})
        assert missing_cpu.is_error,missing_cpu
        missing_irq=await client.call_tool('keyboard_adc_irq_profile',{})
        assert missing_irq.is_error,missing_irq
        no_selector=await client.call_tool('keyboard_connect',{})
        assert no_selector.is_error,no_selector
        connected=await client.call_tool('keyboard_connect',{'demo':True})
        assert not connected.is_error,connected
        assert connected.structured_content['demo'] is True
        assert connected.structured_content['changed_device_settings'] is False
        duplicate=await client.call_tool('keyboard_connect',{'demo':True})
        assert duplicate.is_error,duplicate
        disconnected=await client.call_tool('keyboard_disconnect',{})
        assert not disconnected.is_error,disconnected
        assert disconnected.structured_content['connected'] is False
        assert disconnected.structured_content['changed_device_settings'] is False
        missing=await client.call_tool('keyboard_info',{})
        assert missing.is_error,missing
    print('stdio disconnected startup / explicit demo connect / release PASS')


async def main():
    await disconnected_startup()
    await run(False, 'auto')
    await run(True, 'legacy')


if __name__=='__main__':asyncio.run(main())
