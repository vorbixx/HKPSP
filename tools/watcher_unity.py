"""Type-tree corrections for the pinned copied-original Watcher export."""
from UnityPy.helpers.TypeTreeGenerator import TypeTreeGenerator
from UnityPy.helpers import TypeTreeHelper
TypeTreeHelper.read_typetree_boost = None

class AlignedGenerator(TypeTreeGenerator):
    def get_nodes_up(self,assembly,fullname):
        node=super().get_nodes_up(assembly,fullname)
        def arrays(n):
            children=n.m_Children
            for child in children: arrays(child)
            if children and children[0].m_Type=='Array' and n.m_Type in TypeTreeHelper.FUNCTION_READ_MAP:
                if not (n.m_Type=='string' and children[0].m_Children[1].m_Type=='char'):
                    n.m_Type='vector'
            n.m_Children=children
        arrays(node)
        # Serialized MonoBehaviour header has three padding bytes after enabled.
        # API 0.0.10 omits its alignment flag (raw source header proves offset16).
        children=node.m_Children
        for child in children:
            if child.m_Name=='m_Enabled': child.m_MetaFlag|=0x4000
        node.m_Children=children
        return node
