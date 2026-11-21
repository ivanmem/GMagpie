"""Extract production routing functions; the fixture never loads vendor runtimes."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)


def function(path, signature):
    source = (repo / path).read_text(encoding='utf-8-sig')
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


parts = ['namespace GMagpie {']
parts.extend(function('src/GMagpie.Core/OpticalFlowSettings.h', signature) for signature in [
    'inline MotionVectorRequest ParseOpticalFlowRequest(',
    'inline MotionVectorRequest ParseDlssOpticalFlowRequest('])
for path, signatures in [
    ('src/GMagpie.Core/DLSSNRFilter.cpp', [
        'FrameGuidanceRequirements\nDLSSNRFilter::GetFrameGuidanceRequirements()',
        'EffectParameterRestartReason DLSSNRFilter::GetParameterRestartReason(']),
    ('src/GMagpie.Core/DLSSFrameGenerator.cpp', [
        'FrameGuidanceRequirements\nDLSSFrameGenerator::GetFrameGuidanceRequirements()']),
    ('src/GMagpie.Core/FrameGuidanceDiagnostics.cpp', [
        'FrameGuidanceRequirements\nFrameGuidanceDiagnostics::GetFrameGuidanceRequirements()',
        'EffectParameterApplyMode FrameGuidanceDiagnostics::GetParameterApplyMode(',
        'EffectParameterRestartReason FrameGuidanceDiagnostics::GetParameterRestartReason(',
        'bool FrameGuidanceDiagnostics::ApplyLiveParameters(']),
]:
    parts.extend(function(path, signature) for signature in signatures)
parts.append('}')
(out / 'dlss_optical_flow_production.h').write_text('\n\n'.join(parts), encoding='utf-8')
