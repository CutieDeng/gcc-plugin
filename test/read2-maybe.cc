struct CompactPtr {
  // always points to 4 ints.
  int *elements;
  int short_element;
};

void cutie_init (CompactPtr *self, void *(*malloc) (long long )) {
  self->elements = (int *) malloc (4 * sizeof (int ));
  return ;
}

int read2 (CompactPtr *ptr, int query_type) {
  int ret;
  if (query_type) {
    ret = ptr->elements[1]; 
  } else {
    ret = ptr->short_element;
  }
  return ret;
}

int read2_weak (CompactPtr *ptr, int query_type) {
  int *ret;
  if (query_type) {
    ret = &ptr->elements[1]; 
  } else {
    ret = &ptr->short_element;
  }
  return *ret;
}
