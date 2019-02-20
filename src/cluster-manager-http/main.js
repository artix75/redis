var RedisClusterManager = {
    init: function () {
        var me = this;
        fetch('/nodes').then(function (r) {
            r.json().then(function (nodes) {
                me.nodes = nodes;
                me.onInit();
            });
        });
    },
    onInit: function () {
        var me = this;
        this.masterReplicaContainers = {};
        this.replicas = {};
        this.initialized = true; 
        console.log("Cluster Manager: initialized");
        this.nodeListElem = document.querySelector('#nodes');
        this.nodeTemplate = document.querySelector('template#node-template');
        this.nodes.forEach(function (node) {
            if (node.flags) node.flags = node.flags.split(',');
            if (!node.master_id) {
                var elem = me.renderNode(node, me.nodeListElem);
                var replicaContainer = elem.querySelector('.node-replicas');
                me.masterReplicaContainers[node.id] = replicaContainer;
            } else {
                me.replicas[node.master_id] = 
                    me.replicas[node.master_id] || [];
                me.replicas[node.master_id].push(node);
            }
        });
        Object.keys(this.replicas).forEach(function (master_id) {
            var container = me.masterReplicaContainers[master_id];
            var replicas = me.replicas[master_id];
            if (container) {
                replicas.forEach(function (replica) {
                    me.renderNode(replica, container);
                });
            }
        });
    },
    renderNode: function (node, container) {
        var cloned = this.cloneFromTemplate(this.nodeTemplate);
        var li = cloned.querySelector('li'); 
        var node_id = li.querySelector('.node-id');
        if (node_id) node_id.innerText = node.id;
        var node_addr = li.querySelector('.node-addr');
        if (node_addr) node_addr.innerText = node.ip + ':' + node.port;
        if (node.master_id) li.className += ' replica';
        else li.className += ' master';
        container.appendChild(li);
        return li;
    },
    cloneFromTemplate(templateNode) {
        if ('content' in templateNode) {
            return document.importNode(templateNode.content, true);
        } else {
            var html = templateNode.innerHTML;
            var container = document.createElement('DIV');
            container.innerHTML = html;
            return container.children[0];
        }
    }
};

window.addEventListener('load', function () {
    RedisClusterManager.init();
});
