wayland_client = dependency('wayland-client')

test_input_protocols = executable(
	'test-input-protocols',
	[
		'test-input-protocols.c',
		protocols_code['text-input-unstable-v3'],
		protocols_client_header['text-input-unstable-v3'],
		protocols_code['cursor-shape-v1'],
		protocols_client_header['cursor-shape-v1'],
		protocols_code['tablet-v2'],
	],
	dependencies: [wlroots, wayland_client],
)

test('text-input-v3', test_input_protocols, args: ['text-input'])
test('cursor-shape-v1', test_input_protocols, args: ['cursor-shape'])
