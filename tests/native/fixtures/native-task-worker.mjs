import { runNativeTask } from 'mystral/native-tasks';

self.onmessage = async ({ data }) => {
    try {
        const echoed = await runNativeTask('test.native.echo', data);
        let failure = '';
        try {
            await runNativeTask('test.native.failure', data);
        } catch (error) {
            failure = error && error.message ? error.message : String(error);
        }
        postMessage({ kind: 'native-task-result', echoed, failure });
    } catch (error) {
        postMessage({
            kind: 'native-task-error',
            message: error && error.message ? error.message : String(error),
        });
    }
};
